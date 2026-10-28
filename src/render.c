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

HOT_FN void render_merge(const uint8_t *snap)
{
#if PERSISTENCE_ENABLE
    const uint32_t keep = (uint32_t)(PERSISTENCE_KEEP * 256.0f);
#else
    const uint32_t keep = 0;
#endif
    // The glow update of render_frame() without drawing.
    for (int i = 0; i < GRID_W * GRID_H; i++) {
        uint32_t level = (((s_glow[i] & LEVEL_MASK) * keep) >> 8) & LEVEL_MASK;
        uint32_t shade = s_glow[i] & SHADE_MASK;
        uint32_t st = snap[i];
        if (st) {
            if ((st & LEVEL_MASK) > level) {
                level = st & LEVEL_MASK;
            }
            shade = st & SHADE_MASK;
        }
        s_glow[i] = (uint8_t)(level | shade);
    }
}

// ---- CRT power animation ------------------------------------------------------------

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

void render_crt(uint16_t *fb, float open, float width, float k)
{
    memset(fb, 0, SCREEN_W * SCREEN_H * sizeof(uint16_t));
    open = clamp01(open);
    const int cy = SCREEN_H / 2;
    if (open > 0.0f) {
        // The picture squeezed into a band of height open * SCREEN_H around
        // the middle. Squeezing concentrates the beam: the picture brightens
        // and fills toward a solid line as the band closes.
        const float closed = 1.0f - open;
        const float gain = 1.0f + 1.5f * closed * closed;
        const int floor_level = (int)(40.0f * closed * closed * closed);
        int band = (int)(open * (float)SCREEN_H + 0.5f);
        if (band < 2) {
            band = 2;
        }
        const int top = cy - band / 2;
        for (int y = top; y < top + band; y++) {
            if (y < 0 || y >= SCREEN_H) {
                continue;
            }
            int gy = (int)((float)(y - top) * (float)(GRID_H * GRAIN_PX) / (float)band) / GRAIN_PX;
            gy = gy < 0 ? 0 : gy >= GRID_H ? GRID_H - 1 : gy;
            const uint8_t *glow = &s_glow[gy * GRID_W];
            uint16_t *out = &fb[y * SCREEN_W];
            for (int gx = 0; gx < GRID_W; gx++) {
                int level = (int)((float)(glow[gx] >> 2) * gain) + floor_level;
                level = level > LUT_LEVELS - 1 ? LUT_LEVELS - 1 : level;
                const uint16_t c = s_lut[glow[gx] & SHADE_MASK][level];
                for (int p = 0; p < GRAIN_PX; p++) {
                    *out++ = c;
                }
            }
        }
        return;
    }
    // The beam: a horizontal line `width` of the screen wide around the
    // middle (a dot at 0) with a soft Gaussian edge, at intensity k. Its
    // colour is the palette's brightest shade, running toward white where
    // the beam is hottest.
    k = clamp01(k);
    const uint32_t c = palette_brightest(palette_get(s_palette));
    const float base[3] = { (float)((c >> 16) & 0xFF), (float)((c >> 8) & 0xFF), (float)(c & 0xFF) };
    const float half = 1.0f + clamp01(width) * (float)(SCREEN_W / 2 - 1);
    const float cx = (float)SCREEN_W / 2.0f, mid = (float)cy - 0.5f;
    const int reach = 7;
    for (int y = cy - reach; y <= cy + reach; y++) {
        const float dy = ((float)y - mid) / 1.6f;
        const float vy = expf(-dy * dy);
        for (int x = 0; x < SCREEN_W; x++) {
            float d = fabsf((float)x + 0.5f - cx) - half;
            d = d < 0.0f ? 0.0f : d / 2.5f;
            const float i = k * vy * expf(-d * d);
            if (i < 0.02f) {
                continue;
            }
            const float hot = 0.75f * i;
            int ch[3];
            for (int j = 0; j < 3; j++) {
                const float v = (base[j] + (255.0f - base[j]) * hot) * clamp01(1.3f * i);
                ch[j] = (int)(v > 255.0f ? 255.0f : v);
            }
            fb[y * SCREEN_W + x] = rgb565(ch[0], ch[1], ch[2]);
        }
    }
}

bool render_crt_off_at(uint32_t ms, float *open, float *width, float *k)
{
    const float t = (float)ms;
    *open = 0.0f;
    *width = 0.0f;
    *k = 1.0f;
    if (t < CRT_OFF_SQUEEZE_MS) {
        // Collapse accelerates, like a deflection field dying.
        const float u = t / CRT_OFF_SQUEEZE_MS;
        *open = 1.0f - u * sqrtf(u);
        *width = 1.0f;
    } else if (t < CRT_OFF_SQUEEZE_MS + CRT_OFF_SHRINK_MS) {
        const float u = (t - CRT_OFF_SQUEEZE_MS) / CRT_OFF_SHRINK_MS;
        *width = (1.0f - u) * (1.0f - u);
    } else if (t < CRT_OFF_SQUEEZE_MS + CRT_OFF_SHRINK_MS + CRT_OFF_FADE_MS) {
        const float u = (t - CRT_OFF_SQUEEZE_MS - CRT_OFF_SHRINK_MS) / CRT_OFF_FADE_MS;
        *k = (1.0f - u) * (1.0f - u);
    } else {
        *k = 0.0f;
        return false;
    }
    return true;
}

bool render_crt_on_at(uint32_t ms, float *open, float *width, float *k)
{
    const float t = (float)ms;
    *open = 0.0f;
    *width = 0.0f;
    *k = 1.0f;
    if (t < CRT_ON_DOT_MS) {
        const float u = t / CRT_ON_DOT_MS;
        *k = u * u;
    } else if (t < CRT_ON_DOT_MS + CRT_ON_LINE_MS) {
        const float u = (t - CRT_ON_DOT_MS) / CRT_ON_LINE_MS;
        *width = 1.0f - (1.0f - u) * (1.0f - u);
    } else if (t < CRT_ON_DOT_MS + CRT_ON_LINE_MS + CRT_ON_OPEN_MS) {
        // Opens fast, then eases into the full picture.
        const float u = (t - CRT_ON_DOT_MS - CRT_ON_LINE_MS) / CRT_ON_OPEN_MS;
        *open = 1.0f - (1.0f - u) * (1.0f - u);
        if (*open < 0.01f) {
            *open = 0.01f;
        }
        *width = 1.0f;
    } else {
        *open = 1.0f;
        *width = 1.0f;
        return false;
    }
    return true;
}

void render_progress(uint16_t *fb, float frac, uint32_t rgb)
{
    frac = clamp01(frac);
    const int w = SCREEN_W - 40, h = 10;
    const int x0 = (SCREEN_W - w) / 2, y0 = SCREEN_H / 2 + 34;
    const uint16_t c = render_color(rgb);
    render_rect(fb, x0 - 3, y0 - 3, w + 6, h + 6, 0);
    render_rect(fb, x0 - 2, y0 - 2, w + 4, 1, c);
    render_rect(fb, x0 - 2, y0 + h + 1, w + 4, 1, c);
    render_rect(fb, x0 - 2, y0 - 2, 1, h + 4, c);
    render_rect(fb, x0 + w + 1, y0 - 2, 1, h + 4, c);
    render_rect(fb, x0, y0, (int)((float)w * frac + 0.5f), h, c);
}

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

// ---- Battery OSD --------------------------------------------------------------------

#define BAT_BODY_W   64
#define BAT_BODY_H   128
#define BAT_WALL     3   // outline thickness
#define BAT_INSET    4   // gap between the outline and the bars
#define BAT_GAP      4   // gap between bars
#define BAT_NUB_W    26
#define BAT_NUB_H    8
#define BAT_BOX_PAD  14  // black backing box around the icon

// Add rgb * k (saturating) to a rectangle.
static void add_rect(uint16_t *fb, int x, int y, int w, int h, const int rgb[3], float k)
{
    const int ar = (int)((float)rgb[0] * k), ag = (int)((float)rgb[1] * k), ab = (int)((float)rgb[2] * k);
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= SCREEN_H) {
            continue;
        }
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= SCREEN_W) {
                continue;
            }
            uint16_t *p = &fb[yy * SCREEN_W + xx];
            int r, g, b;
            unpack565(*p, &r, &g, &b);
            r += ar;
            g += ag;
            b += ab;
            *p = rgb565(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
        }
    }
}

// A lit element with a faint 1-px phosphor halo around it.
static void glow_rect(uint16_t *fb, int x, int y, int w, int h, const int rgb[3], float k)
{
    add_rect(fb, x - 1, y - 1, w + 2, 1, rgb, k * 0.3f);
    add_rect(fb, x - 1, y + h, w + 2, 1, rgb, k * 0.3f);
    add_rect(fb, x - 1, y, 1, h, rgb, k * 0.3f);
    add_rect(fb, x + w, y, 1, h, rgb, k * 0.3f);
    add_rect(fb, x, y, w, h, rgb, k);
}

void render_battery(uint16_t *fb, int bars, uint32_t rgb, float alpha)
{
    if (alpha <= 0.0f) {
        return;
    }
    alpha = clamp01(alpha);
    const int col[3] = { (int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF) };
    const int bx = (SCREEN_W - BAT_BODY_W) / 2, by = (SCREEN_H - BAT_BODY_H + BAT_NUB_H) / 2;

    // Backing box: what is behind fades to black as the OSD appears.
    const int x0 = bx - BAT_BOX_PAD, y0 = by - BAT_NUB_H - BAT_BOX_PAD;
    const int x1 = bx + BAT_BODY_W + BAT_BOX_PAD, y1 = by + BAT_BODY_H + BAT_BOX_PAD;
    const int keep = (int)((1.0f - alpha) * 256.0f);
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < SCREEN_H; y++) {
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < SCREEN_W; x++) {
            uint16_t *p = &fb[y * SCREEN_W + x];
            int r, g, b;
            unpack565(*p, &r, &g, &b);
            *p = rgb565((r * keep) >> 8, (g * keep) >> 8, (b * keep) >> 8);
        }
    }

    // Outline and terminal nub.
    glow_rect(fb, bx, by, BAT_BODY_W, BAT_WALL, col, alpha);
    glow_rect(fb, bx, by + BAT_BODY_H - BAT_WALL, BAT_BODY_W, BAT_WALL, col, alpha);
    glow_rect(fb, bx, by + BAT_WALL, BAT_WALL, BAT_BODY_H - 2 * BAT_WALL, col, alpha);
    glow_rect(fb, bx + BAT_BODY_W - BAT_WALL, by + BAT_WALL, BAT_WALL, BAT_BODY_H - 2 * BAT_WALL, col, alpha);
    glow_rect(fb, bx + (BAT_BODY_W - BAT_NUB_W) / 2, by - BAT_NUB_H, BAT_NUB_W, BAT_NUB_H, col, alpha);

    // Five bars, bottom up; unlit ones are a faint trace of their slot.
    const int ix = bx + BAT_WALL + BAT_INSET, iw = BAT_BODY_W - 2 * (BAT_WALL + BAT_INSET);
    const int itop = by + BAT_WALL + BAT_INSET, ih = BAT_BODY_H - 2 * (BAT_WALL + BAT_INSET);
    const int bh = (ih - 4 * BAT_GAP) / 5;
    const int spare = ih - 5 * bh - 4 * BAT_GAP; // centre the stack
    for (int i = 0; i < 5; i++) {
        const int y = itop + spare / 2 + (4 - i) * (bh + BAT_GAP);
        if (i < bars) {
            glow_rect(fb, ix, y, iw, bh, col, alpha * 0.85f);
        } else {
            add_rect(fb, ix, y, iw, bh, col, alpha * 0.08f);
        }
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
