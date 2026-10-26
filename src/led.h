// Onboard WS2812B: soft pulse in the palette colour, brighter with sand motion.
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t led_init(void);

// color: 0xRRGGBB at full brightness. activity: 0..1 (sand motion).
void led_update(uint32_t color, float activity, uint32_t now_ms);
