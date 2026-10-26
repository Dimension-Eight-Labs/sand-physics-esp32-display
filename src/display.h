// ST7789 panel over SPI with asynchronous full-frame DMA pushes.
#pragma once

#include <stdint.h>

#include "esp_err.h"

// Initialise SPI bus, panel and backlight; allocates the frame buffer in
// DMA-capable internal RAM.
esp_err_t display_init(void);

// SCREEN_W x SCREEN_H RGB565 frame buffer (panel byte order).
uint16_t *display_framebuffer(void);

// Start sending the whole frame buffer. Returns immediately; the buffer must
// not be written until display_wait() returns.
void display_push_async(void);

// Block until the previous push (if any) has finished.
void display_wait(void);

// 0..100 %.
void display_set_backlight(int percent);
