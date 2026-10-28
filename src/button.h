// BOOT button: polled every GESTURE_POLL_MS from an esp_timer, gestures from
// src/gesture.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gesture.h"

esp_err_t button_init(void);

// GESTURE_* events since the last call, then cleared.
uint32_t button_take_events(void);

// Hold-to-power-off progress (gesture_hold_progress()): < 0 = no indicator.
float button_hold_progress(void);

// True while BOOT is pressed (raw level).
bool button_is_down(void);
