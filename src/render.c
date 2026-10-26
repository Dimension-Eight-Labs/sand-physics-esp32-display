// Phosphor-style renderer.
//
// The look is computed at grid resolution. Each cell holds one byte: a glow
// level (top 6 bits) and the shade of the last grain that lit it (low 2 bits).
//
// Two halves, which may run on different cores:
//  - render_snapshot() (simulation side) packs every grain into a per-cell
//    byte map at a brightness that grows with its speed.
//  - render_frame() (display side) decays the glow toward black
//    (persistence), merges the snapshot, and expands cells to
//    GRAIN_PX x GRAIN_PX pixels through a pre-computed RGB565 table.
#include "render.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "material.h"
#include "palette.h"

#define LUT_LEVELS 64 // glow level >> 2
#define LEVEL_MASK 0xFC
#define SHADE_MASK 0x03
_Static_assert(PALETTE_SHADES <= SHADE_MASK + 1, "shade must fit in 2 bits");

// Speed -> brightness, indexed by speed^2 so no square root per grain. It spans
// 0..SPEED_LUT_SPAN cells/step; the glow saturates at (255 - GLOW_BASE) /
// GLOW_SPEED_GAIN = 0.75 cells/step, so faster grains (sand flies up to 4)
// share the last entry and the table stays as it was for gravel and liquid.
#define SPEED_LUT_N     256
#define SPEED_LUT_SPAN  2.0f
static float s_speed_scale; // (SPEED_LUT_N - 1) / max_speed^2

static uint8_t s_glow[GRID_W * GRID_H]; // level | shade, see above
static uint16_t s_lut[PALETTE_SHADES][LUT_LEVELS];
static uint8_t s_speed_lut[SPEED_LUT_N];
static uint16_t s_row[SCREEN_W];
static int s_palette;

static inline uint16_t to_panel(uint16_t v)
{
#if BOARD_LCD_SWAP_BYTES
    return (uint16_t)((v >> 8) | (v << 8));
#else
    return v;
#endif
}

static inline uint16_t rgb565(int r, int g, int b)
{
    return to_panel((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)));
}

uint16_t render_color(uint32_t rgb)
{
    return rgb565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// spread: 1 = the palette's shades as they are, 0 = every shade = shade 0.
static void build_lut(const palette_t *p, float spread)
{
    const float base = (float)GLOW_BASE / 255.0f;
    const float p0[3] = {
        (float)((p->shade[0] >> 16) & 0xFF), (float)((p->shade[0] >> 8) & 0xFF), (float)(p->shade[0] & 0xFF),
    };
    for (int s = 0; s < PALETTE_SHADES; s++) {
        float r0 = p0[0] + ((float)((p->shade[s] >> 16) & 0xFF) - p0[0]) * spread;
        float g0 = p0[1] + ((float)((p->shade[s] >> 8) & 0xFF) - p0[1]) * spread;
        float b0 = p0[2] + ((float)(p->shade[s] & 0xFF) - p0[2]) * spread;
        for (int l = 0; l < LUT_LEVELS; l++) {
            float t = (float)l / (float)(LUT_LEVELS - 1);
            float i = powf(t, GLOW_GAMMA);
            // Above the settled level, blend toward white like an overdriven spot.
            float hot = 0.0f;
            if (t > base) {
                hot = (t - base) / (1.0f - base) * HOT_WHITE_MIX;
            }
            float r = r0 * i, g = g0 * i, b = b0 * i;
            r += (255.0f * i - r) * hot;
            g += (255.0f * i - g) * hot;
            b += (255.0f * i - b) * hot;
            s_lut[s][l] = rgb565((int)(r + 0.5f), (int)(g + 0.5f), (int)(b + 0.5f));
        }
        s_lut[s][0] = 0; // exact black background
    }
}

void render_init(int palette)
{
    s_speed_scale = (float)(SPEED_LUT_N - 1) / (SPEED_LUT_SPAN * SPEED_LUT_SPAN);
    for (int i = 0; i < SPEED_LUT_N; i++) {
        float speed = sqrtf((float)i / s_speed_scale);
        float level = (float)GLOW_BASE + GLOW_SPEED_GAIN * speed;
        int l = level > 255.0f ? 255 : (int)level;
        // Never encode as 0, which means "no grain" in a snapshot.
        s_speed_lut[i] = (uint8_t)(l < 4 ? 4 : l) & LEVEL_MASK;
    }
    render_set_look(palette, 1.0f);
    render_clear();
}

void render_set_look(int palette, float shade_spread)
{
    s_palette = ((palette % PALETTE_COUNT) + PALETTE_COUNT) % PALETTE_COUNT;
    build_lut(palette_get(s_palette), shade_spread);
}

int render_palette(void)
{
    return s_palette;
}

void render_clear(void)
{
    memset(s_glow, 0, sizeof(s_glow));
}

HOT_FN void render_snapshot(const sim_t *s, uint8_t *snap)
{
    memset(snap, 0, (size_t)s->w * s->h);
    for (int i = 0; i < s->n; i++) {
        const sim_grain_t *g = &s->grains[i];
        int idx = (int)g->y * s->w + (int)g->x;
        int k = (int)(sim_grain_speed2(g) * s_speed_scale);
        if (k >= SPEED_LUT_N) {
            k = SPEED_LUT_N - 1;
        }
        snap[idx] = (uint8_t)(s_speed_lut[k] | s->shade[i]);
    }
}

HOT_FN void render_frame(const uint8_t *snap, uint16_t *fb)
{
#if PERSISTENCE_ENABLE
    const uint32_t keep = (uint32_t)(PERSISTENCE_KEEP * 256.0f);
#else
    const uint32_t keep = 0;
#endif

    // Columns/rows not covered by the grid stay black.
    memset(s_row, 0, sizeof(s_row));
    for (int gy = 0; gy < GRID_H; gy++) {
        uint8_t *glow = &s_glow[gy * GRID_W];
        const uint8_t *stamp = &snap[gy * GRID_W];
        uint16_t *out = s_row;
        for (int gx = 0; gx < GRID_W; gx++) {
            // Decay the afterglow, then let a grain in this cell relight it.
            uint32_t level = (((glow[gx] & LEVEL_MASK) * keep) >> 8) & LEVEL_MASK;
            uint32_t shade = glow[gx] & SHADE_MASK;
            uint32_t st = stamp[gx];
            if (st) {
                if ((st & LEVEL_MASK) > level) {
                    level = st & LEVEL_MASK;
                }
                shade = st & SHADE_MASK;
            }
            glow[gx] = (uint8_t)(level | shade);
            uint16_t c = s_lut[shade][level >> 2];
            for (int k = 0; k < GRAIN_PX; k++) {
                *out++ = c;
            }
        }
        uint16_t *dst = &fb[gy * GRAIN_PX * SCREEN_W];
        for (int k = 0; k < GRAIN_PX; k++) {
            memcpy(dst + k * SCREEN_W, s_row, sizeof(s_row));
        }
    }
    int covered = GRID_H * GRAIN_PX;
    if (covered < SCREEN_H) {
        memset(&fb[covered * SCREEN_W], 0, (size_t)(SCREEN_H - covered) * SCREEN_W * sizeof(uint16_t));
    }
}

// ---- Text overlay -----------------------------------------------------------------

// Classic 5x7 terminal font, A-Z. One byte per row, top row first; bit 4 is
// the leftmost column.
static const uint8_t FONT_5X7[26][7] = {
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // A
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, // B
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, // C
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, // D
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, // E
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, // F
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, // G
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // H
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, // I
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, // J
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, // K
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, // L
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, // M
    { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, // N
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // O
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, // P
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, // Q
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, // R
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, // S
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, // T
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // U
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, // V
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, // W
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, // X
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, // Y
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, // Z
};

// Panel-order RGB565 -> 8-bit channels and back.
static inline void unpack565(uint16_t c, int *r, int *g, int *b)
{
#if BOARD_LCD_SWAP_BYTES
    c = (uint16_t)((c >> 8) | (c << 8));
#endif
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >> 5) & 0x3F) << 2;
    *b = (c & 0x1F) << 3;
}

static int glyph_index(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a';
    }
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    return -1; // drawn as a space
}

void render_overlay(uint16_t *fb, const char *text, uint32_t rgb, float alpha)
{
    if (alpha <= 0.0f || !text || !text[0]) {
        return;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    const int len = (int)strlen(text);
    // Largest scale up to OVERLAY_SCALE that fits the screen width.
    int scale = OVERLAY_SCALE;
    while (scale > 1 && len * 6 * scale - scale + 2 * OVERLAY_PAD > SCREEN_W) {
        scale--;
    }
    const int text_w = len * 6 * scale - scale; // 5 columns + 1 gap per glyph, no trailing gap
    const int text_h = 7 * scale;
    const int box_w = text_w + 2 * OVERLAY_PAD, box_h = text_h + 2 * OVERLAY_PAD;
    const int bx = (SCREEN_W - box_w) / 2, by = (SCREEN_H - box_h) / 2;
    const int tx = bx + OVERLAY_PAD, ty = by + OVERLAY_PAD;
    const int keep = (int)((1.0f - alpha) * 256.0f); // background kept, /256
    const int tr = (int)((float)((rgb >> 16) & 0xFF) * alpha);
    const int tg = (int)((float)((rgb >> 8) & 0xFF) * alpha);
    const int tb = (int)((float)(rgb & 0xFF) * alpha);

    for (int y = by; y < by + box_h; y++) {
        if (y < 0 || y >= SCREEN_H) {
            continue;
        }
        const int gy = (y - ty) / scale; // glyph row, valid if 0..6
        for (int x = bx; x < bx + box_w; x++) {
            if (x < 0 || x >= SCREEN_W) {
                continue;
            }
            uint16_t *p = &fb[y * SCREEN_W + x];
            int r, g, b;
            unpack565(*p, &r, &g, &b);
            // Backing box: the grains behind fade to black as the text appears.
            r = (r * keep) >> 8;
            g = (g * keep) >> 8;
            b = (b * keep) >> 8;
            if (y >= ty && y < ty + text_h && x >= tx && x < tx + text_w) {
                const int cx = (x - tx) / scale;
                const int col = cx % 6, gi = glyph_index(text[cx / 6]);
                if (col < 5 && gi >= 0 && (FONT_5X7[gi][gy] >> (4 - col)) & 1) {
                    r += tr;
                    g += tg;
                    b += tb;
                }
            }
            *p = rgb565(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
        }
    }
}

// ---- Bring-up helpers ----------------------------------------------------------

void render_fill(uint16_t *fb, uint16_t color)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        fb[i] = color;
    }
}

void render_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= SCREEN_H) {
            continue;
        }
        for (int xx = x; xx < x + w; xx++) {
            if (xx >= 0 && xx < SCREEN_W) {
                fb[yy * SCREEN_W + xx] = color;
            }
        }
    }
}

void render_test_pattern(uint16_t *fb)
{
    // Eight horizontal bars, top to bottom: white, yellow, cyan, green,
    // magenta, red, blue, black.
    static const uint32_t bars[8] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
    };
    for (int i = 0; i < 8; i++) {
        int y0 = SCREEN_H * i / 8, y1 = SCREEN_H * (i + 1) / 8;
        render_rect(fb, 0, y0, SCREEN_W, y1 - y0, render_color(bars[i]));
    }
    // Grey ramp inside the black bar to judge gamma and inversion.
    // Starts right of the bottom-left marker.
    int y0 = SCREEN_H * 7 / 8 + 8;
    for (int x = 30; x < SCREEN_W - 8; x++) {
        int v = (x - 30) * 255 / (SCREEN_W - 39);
        render_rect(fb, x, y0, 1, 16, rgb565(v, v, v));
    }
    // Orientation markers: red = top-left, green = top-right, blue = bottom-left.
    render_rect(fb, 4, 4, 20, 20, render_color(0xFF0000));
    render_rect(fb, SCREEN_W - 24, 4, 20, 20, render_color(0x00FF00));
    render_rect(fb, 4, SCREEN_H - 24, 20, 20, render_color(0x0000FF));
    // 1-pixel white border: all four edges must be visible if the offset is right.
    uint16_t w = render_color(0xFFFFFF);
    render_rect(fb, 0, 0, SCREEN_W, 1, w);
    render_rect(fb, 0, SCREEN_H - 1, SCREEN_W, 1, w);
    render_rect(fb, 0, 0, 1, SCREEN_H, w);
    render_rect(fb, SCREEN_W - 1, 0, 1, SCREEN_H, w);
}
