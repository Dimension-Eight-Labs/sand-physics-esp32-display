// Battery level from voltage: LiPo discharge curve, smoothing, hysteresis.
// Hardware-free (tools/sim_battery.c checks it); src/battery.c feeds it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float mv;          // filtered cell voltage, mV (0 = no reading yet)
    float mv_fast;     // lightly filtered, for the charging hint
    int n;             // readings so far
    int percent;       // shown percentage, 0..100, with hysteresis
    int bars;          // shown bars, 0..5, with hysteresis
    bool charging;     // above BAT_CHARGING_MV: on the charger / USB
    bool low;          // below BAT_LOW_PERCENT (with hysteresis), not charging
    bool valid;        // at least one plausible reading
} batt_t;

void batt_init(batt_t *b);

// One averaged reading (mV at the cell) taken dt_s after the previous one.
void batt_update(batt_t *b, float cell_mv, float dt_s);

// Single-cell LiPo state of charge for a cell voltage (curve lookup,
// linear between points), 0..100.
float batt_percent_from_mv(float mv);

// Low-voltage cutoff for a settled reading `mv`; after_cutoff: the last
// power-off was a cutoff, so the cell must have recovered by
// BAT_CUTOFF_RESUME_MV to run again. False when BAT_CUTOFF_ENABLE is 0.
bool batt_below_cutoff(float mv, bool after_cutoff);

// Average drain in mV per day from mv_then to mv_now over `seconds`
// (positive = the cell lost charge).
float batt_drain_mv_per_day(float mv_then, float mv_now, double seconds);
