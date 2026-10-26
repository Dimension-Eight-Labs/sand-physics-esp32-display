// Frame composition: sim grid -> phosphor glow buffer -> RGB565 frame buffer.
// Hardware-free; the frame buffer is SCREEN_W x SCREEN_H pixels in panel byte
// order (see BOARD_LCD_SWAP_BYTES).
#pragma once

#include <stdint.h>

#include "sim.h"

// Build the speed table for all materials, select the palette at full shade
// variation and clear the glow buffer.
void render_init(int palette);

// Palette plus shade variation (material shade_spread: 1 = palette as is,
// smaller = shades pulled toward the primary colour). Rebuilds the colour table.
void render_set_look(int palette, float shade_spread);
int render_palette(void);

// Forget all afterglow (e.g. on respawn).
void render_clear(void);

// Simulation side: pack the grains into a GRID_W x GRID_H byte map (0 = empty,
// else brightness level | shade). Only reads `s` and the speed table built by
// render_init(), so it can run on the simulation's core.
void render_snapshot(const sim_t *s, uint8_t *snap);

// Display side: decay the afterglow, merge a snapshot and write the full frame.
void render_frame(const uint8_t *snap, uint16_t *fb);

// Centered text in a blocky 5x7 terminal font, scaled up (OVERLAY_SCALE, or
// smaller if it would not fit), over a backing box that darkens what is
// behind it. alpha 0..1 fades box and text together. A-Z only (case
// ignored); anything else is a space.
void render_overlay(uint16_t *fb, const char *text, uint32_t rgb, float alpha);

// Bring-up helpers.
void render_test_pattern(uint16_t *fb);
void render_fill(uint16_t *fb, uint16_t color);
void render_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);

// 0xRRGGBB -> RGB565 in panel byte order.
uint16_t render_color(uint32_t rgb);
