// Onboard WS2812B: soft pulse in the palette colour, brighter with sand motion.
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t led_init(void);

// color: 0xRRGGBB at full brightness, gamma-encoded like a screen colour
// (linearised with LED_GAMMA). activity: 0..1 (sand motion). scale: overall
// brightness factor 0..1 (idle dimming, power animations).
void led_update(uint32_t color, float activity, float scale, uint32_t now_ms);

// Drive exactly `color` (gamma-encoded) at `level` (0..255), no pulse.
void led_show(uint32_t color, int level);

// The 0..255 channel values led_show() / led_update() send for `color` at
// `level` (after gamma), for logging.
uint32_t led_linear(uint32_t color, int level);

// For deep sleep: LED dark, RMT released, data line held at its sleep level
// (SLEEP_LED_DATA_LOW).
void led_sleep(void);

// Just the pin part of led_sleep() (again after a brief wake that did not
// start the LED).
void led_hold_pin(void);
