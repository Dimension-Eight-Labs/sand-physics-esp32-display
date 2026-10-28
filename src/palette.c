// Phosphor palettes. The first shade of each is the classic reference colour;
// the others are slight variations so piles get some texture. Tune by eye.
#include "palette.h"

#include "config.h"

static const palette_t PALETTES[PALETTE_COUNT] = {
    [PALETTE_GREEN] = {
        .name = "green",
        .shade = { 0x33FF33, 0x2BD92B, 0x5CFF5C, 0x22B822 },
        .led = LED_COLOR_GREEN,
    },
    // P3-style amber phosphor; the orange of the demo.
    [PALETTE_ORANGE] = {
        .name = "orange",
        .shade = { 0xFFB000, 0xE69A00, 0xFFC233, 0xCC8800 },
        .led = LED_COLOR_ORANGE,
    },
};

const palette_t *palette_get(int idx)
{
    idx %= PALETTE_COUNT;
    if (idx < 0) {
        idx += PALETTE_COUNT;
    }
    return &PALETTES[idx];
}

uint32_t palette_brightest(const palette_t *p)
{
    uint32_t best = p->shade[0];
    int best_y = -1;
    for (int i = 0; i < PALETTE_SHADES; i++) {
        uint32_t c = p->shade[i];
        int y = (int)((c >> 16) & 0xFF) * 299 + (int)((c >> 8) & 0xFF) * 587 + (int)(c & 0xFF) * 114;
        if (y > best_y) {
            best_y = y;
            best = c;
        }
    }
    return best;
}
