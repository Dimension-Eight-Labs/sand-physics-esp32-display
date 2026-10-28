// Battery voltage on BAT_ADC: sampled by a low-priority task on core 1,
// level from src/batt.c.
#pragma once

#include "esp_err.h"

#include "batt.h"

// ADC + calibration, first reading, then the sampling task.
esp_err_t battery_init(void);

// A settled reading now, mV at the cell (< 0 on error): a few conversions
// discarded, BAT_SETTLE_MS wait, then BAT_SETTLED_SAMPLES averaged, raw (not
// the smoothed level). About 30 ms; blocks.
float battery_read_settled_mv(void);

// The settled reading battery_init() took at boot (< 0 if none).
float battery_boot_mv(void);

// Latest state (all zero / invalid before the first reading).
void battery_get(batt_t *out);

// "curve fitting", "none (uncalibrated)", for the banner.
const char *battery_calibration(void);
