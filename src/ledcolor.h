// RGB LED colour maths, hardware-free (tools/sim_ledcolor.c checks it).
//
// Colours are plain 0xRRGGBB, gamma-encoded like a screen colour. Any
// adjustment (ledcal's hue / saturation steps) happens on that encoded value;
// then each channel is linearised once with LED_GAMMA, and brightness (level,
// pulse, dimming, animations) multiplies the linear channels together, so it
// never changes the hue. Channel order on the wire is the driver's business
// (BOARD_RGB_ORDER_RGB in board.h), not this module's.
#pragma once

#include <stdint.h>

// `color` at brightness `level` (0..255, fractional allowed): the 0..255
// R, G, B values to send, as 0xRRGGBB.
uint32_t ledcolor_drive(uint32_t color, float level);

// Gamma-encoded HSV (h in degrees, s and v 0..1) <-> 0xRRGGBB.
void ledcolor_to_hsv(uint32_t c, float *h, float *s, float *v);
uint32_t ledcolor_from_hsv(float h, float s, float v);
