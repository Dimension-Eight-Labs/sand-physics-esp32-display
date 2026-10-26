// BOOT button: debounced short / long press events.
#pragma once

#include <stdint.h>

#include "esp_err.h"

#define BUTTON_EVT_SHORT (1u << 0) // released before BTN_LONG_MS
#define BUTTON_EVT_LONG  (1u << 1) // held for BTN_LONG_MS (fires while still held)

esp_err_t button_init(void);

// Events since the last call (BUTTON_EVT_* bits), then cleared.
uint32_t button_take_events(void);
