// Monochrome phosphor palettes. Hardware-free.
#pragma once

#include <stdint.h>

#define PALETTE_SHADES 4

typedef struct {
    const char *name;
    uint32_t shade[PALETTE_SHADES]; // 0xRRGGBB at full brightness; grains pick one at spawn
    uint32_t led;                   // 0xRRGGBB for the onboard RGB LED
} palette_t;

// idx is taken modulo PALETTE_COUNT.
const palette_t *palette_get(int idx);
