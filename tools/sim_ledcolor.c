// Host test: RGB LED colour maths (src/ledcolor.c) (no ESP-IDF needed).
//
//   mkdir -p build-host && cc -std=c11 -O2 -Isrc src/ledcolor.c src/palette.c tools/sim_ledcolor.c -lm
//       -o build-host/sim_ledcolor && build-host/sim_ledcolor
//
// Checks, for the palette LED colours, pure primaries and every ledcal
// candidate:
//  - channels stay in place: what is sent for 0xRRGGBB has R in R, G in G
//    (byte order on the wire is the driver's, set by BOARD_RGB_ORDER_RGB);
//  - gamma is applied once per channel: each sent channel equals
//    255 * (c / 255)^LED_GAMMA * level / 255 within rounding;
//  - dimming changes brightness only: at every level each sent channel is its
//    linear value times the level (rounded), so the hue cannot change.
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "config.h"
#include "ledcolor.h"
#include "palette.h"

static int s_fail;

static float hue_of(uint32_t c)
{
    float h, s, v;
    ledcolor_to_hsv(c, &h, &s, &v);
    return h;
}

static void check(const char *name, uint32_t color)
{
    // Gamma once, per channel, at full level.
    const uint32_t full = ledcolor_drive(color, 255.0f);
    float worst_gamma = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float enc = (float)((color >> (16 - 8 * i)) & 0xFF);
        const float want = 255.0f * powf(enc / 255.0f, LED_GAMMA);
        const float got = (float)((full >> (16 - 8 * i)) & 0xFF);
        worst_gamma = fmaxf(worst_gamma, fabsf(got - want));
    }
    // Brightness: at every level from LED_MAX_LEVEL down to 1 each sent
    // channel is its linear value times the level, rounded, so brightness is a
    // pure scale of all three channels together and the hue cannot change
    // (beyond 8-bit rounding).
    float worst_scale = 0.0f;
    int levels = 0;
    for (float level = (float)LED_MAX_LEVEL; level >= 1.0f; level *= 0.9f, levels++) {
        const uint32_t d = ledcolor_drive(color, level);
        for (int i = 0; i < 3; i++) {
            const float lin = powf((float)((color >> (16 - 8 * i)) & 0xFF) / 255.0f, LED_GAMMA);
            worst_scale = fmaxf(worst_scale, fabsf((float)((d >> (16 - 8 * i)) & 0xFF) - lin * level));
        }
    }
    const float h_full = hue_of(full);
    const bool ok = worst_gamma <= 0.5f && worst_scale <= 0.5f;
    const uint32_t at = ledcolor_drive(color, (float)LED_MAX_LEVEL);
    printf("  %-24s 0x%06" PRIX32 " -> sent R%3u G%3u B%3u at level %d; gamma error %.2f; hue %.1f deg; "
           "brightness scaling error %.2f over %d levels  %s\n",
           name, color, (unsigned)(at >> 16), (unsigned)((at >> 8) & 0xFF), (unsigned)(at & 0xFF), LED_MAX_LEVEL,
           (double)worst_gamma, (double)h_full, (double)worst_scale, levels, ok ? "ok" : "FAIL");
    s_fail += !ok;
}

int main(void)
{
    printf("LED colour: gamma %.1f, byte order on the wire %s (board.h, applied by the driver only)\n",
           (double)LED_GAMMA, BOARD_RGB_ORDER_RGB ? "RGB" : "GRB");
    check("pure red", 0xFF0000);
    check("pure green", 0x00FF00);
    check("pure blue", 0x0000FF);
    for (int p = 0; p < PALETTE_COUNT; p++) {
        char name[32];
        snprintf(name, sizeof(name), "%s palette LED", palette_get(p)->name);
        check(name, palette_get(p)->led);
    }
    // Every ledcal candidate (same stepping as main.c).
    int n = 0, bad = s_fail;
    for (int p = 0; p < PALETTE_COUNT; p++) {
        float h, s, v;
        ledcolor_to_hsv(palette_get(p)->led, &h, &s, &v);
        for (int si = 0; si <= LEDCAL_SAT_STEPS; si++) {
            for (int hi = -LEDCAL_HUE_STEPS; hi <= LEDCAL_HUE_STEPS; hi++) {
                const float hh = h + (float)hi * LEDCAL_HUE_STEP_DEG, ss = fmaxf(0.0f, s - (float)si * LEDCAL_SAT_STEP);
                const uint32_t c = ledcolor_from_hsv(hh, ss, v);
                // The candidate is the hue asked for (round trip through 8 bits).
                float dh = fabsf(hue_of(c) - fmodf(hh + 360.0f, 360.0f));
                dh = dh > 180.0f ? 360.0f - dh : dh;
                if (dh > 1.0f) {
                    printf("  candidate %s hue %.0f -> 0x%06" PRIX32 " has hue %.1f  FAIL\n", palette_get(p)->name,
                           (double)hh, c, (double)hue_of(c));
                    s_fail++;
                }
                const uint32_t d = ledcolor_drive(c, (float)LEDCAL_LEVEL);
                // Red never exceeds green for a candidate whose encoded red is below its green.
                if (((c >> 16) & 0xFF) < ((c >> 8) & 0xFF) && (d >> 16) > ((d >> 8) & 0xFF)) {
                    printf("  candidate 0x%06" PRIX32 " sent with red above green  FAIL\n", c);
                    s_fail++;
                }
                n++;
            }
        }
    }
    printf("  %d ledcal candidates: hue round trip and channel order %s\n", n, s_fail == bad ? "ok" : "FAIL");
    printf("%s\n", s_fail ? "LED COLOUR FAILURES" : "all LED colour checks ok");
    return s_fail != 0;
}
