// RGB LED colour maths; see ledcolor.h.
#include "ledcolor.h"

#include <math.h>

#include "config.h"

uint32_t ledcolor_drive(uint32_t color, float level)
{
    if (level < 0.0f) {
        level = 0.0f;
    }
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        const float enc = (float)((color >> (16 - 8 * i)) & 0xFF) / 255.0f;
        const float v = powf(enc, LED_GAMMA) * level + 0.5f; // gamma once, then brightness
        out = (out << 8) | (uint32_t)(v > 255.0f ? 255.0f : v);
    }
    return out;
}

void ledcolor_to_hsv(uint32_t c, float *h, float *s, float *v)
{
    const float r = (float)((c >> 16) & 0xFF) / 255.0f, g = (float)((c >> 8) & 0xFF) / 255.0f,
                b = (float)(c & 0xFF) / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        *h = 0.0f;
    } else if (mx == r) {
        *h = 60.0f * fmodf((g - b) / d + 6.0f, 6.0f);
    } else if (mx == g) {
        *h = 60.0f * ((b - r) / d + 2.0f);
    } else {
        *h = 60.0f * ((r - g) / d + 4.0f);
    }
}

uint32_t ledcolor_from_hsv(float h, float s, float v)
{
    h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f);
    const float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r, g, b;
    if (h < 60.0f) {
        r = c, g = x, b = 0.0f;
    } else if (h < 120.0f) {
        r = x, g = c, b = 0.0f;
    } else if (h < 180.0f) {
        r = 0.0f, g = c, b = x;
    } else if (h < 240.0f) {
        r = 0.0f, g = x, b = c;
    } else if (h < 300.0f) {
        r = x, g = 0.0f, b = c;
    } else {
        r = c, g = 0.0f, b = x;
    }
    return ((uint32_t)lroundf((r + m) * 255.0f) << 16) | ((uint32_t)lroundf((g + m) * 255.0f) << 8) |
           (uint32_t)lroundf((b + m) * 255.0f);
}
