// Phosphor palettes. The first shade of each is the classic reference colour;
// the others are slight variations so piles get some texture. Tune by eye.
#include "palette.h"

#include "config.h"

static const palette_t PALETTES[PALETTE_COUNT] = {
    [PALETTE_P1_GREEN] = {
        .name = "P1 green",
        .shade = { 0x33FF33, 0x2BD92B, 0x5CFF5C, 0x22B822 },
        .led = 0x00FF10,
    },
    [PALETTE_P3_AMBER] = {
        .name = "P3 amber",
        .shade = { 0xFFB000, 0xE69A00, 0xFFC233, 0xCC8800 },
        .led = 0xFF6000,
    },
    // Deeper, redder orange like a neon gas-plasma display.
    [PALETTE_ORANGE] = {
        .name = "orange",
        .shade = { 0xFF6A00, 0xE65A00, 0xFF8533, 0xC24E00 },
        .led = 0xFF2800,
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
