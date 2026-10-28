// Monochrome phosphor palettes. Hardware-free.
#pragma once

#include <stdint.h>

#define PALETTE_SHADES 4

typedef struct {
    const char *name;
    uint32_t shade[PALETTE_SHADES]; // 0xRRGGBB at full brightness; grains pick one at spawn
    uint32_t led;                   // 0xRRGGBB for the onboard RGB LED (config.h LED_COLOR_*)
} palette_t;

// idx is taken modulo PALETTE_COUNT.
const palette_t *palette_get(int idx);

// The palette's brightest shade (by perceived luminance), 0xRRGGBB.
uint32_t palette_brightest(const palette_t *p);
