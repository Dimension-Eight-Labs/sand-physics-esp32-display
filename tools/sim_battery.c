// Host test: battery level model (src/batt.c) (no ESP-IDF needed).
//
//   mkdir -p build-host && cc -std=c11 -O2 -Isrc src/batt.c tools/sim_battery.c -lm
//       -o build-host/sim_battery && build-host/sim_battery
//
//  - the discharge curve: percent at sample voltages, monotonic;
//  - noise: a cell held at fixed voltages, read once per second with
//    +/-40 mV of noise (about 1 % ADC error at the cell, x3 divider) for
//    10 minutes: how often the shown percent, bars and low flag change;
//  - a slow discharge 4.20 -> 3.30 V over 3 hours with the same noise: the
//    shown values only go down, and when each bar / the low flag changes;
//  - charging: 4.22 V (on USB, full cell or no cell) and back;
//  - the low-voltage cutoff thresholds and the drain-rate estimate.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "batt.h"
#include "config.h"

static unsigned s_seed = 1;

static float noise(float amp)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return amp * ((float)((s_seed >> 8) & 0xFFFF) / 32767.5f - 1.0f);
}

static int s_fail;

int main(void)
{
    printf("curve: ");
    float prev = -1.0f;
    for (int mv = 3200; mv <= 4250; mv += 50) {
        const float p = batt_percent_from_mv((float)mv);
        printf("%d:%.0f ", mv, (double)p);
        if (p < prev) {
            s_fail++;
            printf("(NOT MONOTONIC) ");
        }
        prev = p;
    }
    printf("\n");

    printf("held still, +/-40 mV noise, 1 reading/s for 10 min (hysteresis %d %%, filter %.0f s):\n", BAT_HYST_PERCENT,
           (double)BAT_FILTER_S);
    const float held[] = { 4150, 4000, 3850, 3800, 3740, 3711, 3700, 3650 };
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        batt_t b;
        batt_init(&b);
        int pc = 0, bc = 0, lc = 0, last_p = -1, last_b = -1, last_l = -1, pmin = 101, pmax = -1;
        for (int t = 0; t < 600; t++) {
            batt_update(&b, held[i] + noise(40.0f), 1.0f);
            if (t >= 30) { // after the filter has settled
                pc += last_p >= 0 && b.percent != last_p;
                bc += last_b >= 0 && b.bars != last_b;
                lc += last_l >= 0 && b.low != last_l;
                pmin = b.percent < pmin ? b.percent : pmin;
                pmax = b.percent > pmax ? b.percent : pmax;
                last_p = b.percent;
                last_b = b.bars;
                last_l = b.low;
            }
        }
        const bool ok = pc <= 2 && bc == 0 && lc == 0;
        printf("  %4.0f mV (%3.0f %%): shown %d..%d %%, %d bars, low %d; changes: percent %d, bars %d, low %d  %s\n",
               (double)held[i], (double)batt_percent_from_mv(held[i]), pmin, pmax, b.bars, b.low, pc, bc, lc,
               ok ? "ok" : "FAIL");
        s_fail += !ok;
    }

    printf("discharge 4.20 -> 3.30 V over 3 h, +/-40 mV noise:\n");
    {
        batt_t b;
        batt_init(&b);
        int last_b = -1, last_p = 101, ups = 0, last_low = 0;
        const int total = 3 * 3600;
        for (int t = 0; t <= total; t++) {
            // Through the curve evenly in state of charge (voltage follows the curve).
            const float soc = 100.0f * (1.0f - (float)t / (float)total);
            float lo = 3270.0f, hi = 4200.0f;
            for (int k = 0; k < 30; k++) {
                const float mid = 0.5f * (lo + hi);
                if (batt_percent_from_mv(mid) < soc) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            batt_update(&b, 0.5f * (lo + hi) + noise(40.0f), 1.0f);
            if (b.percent > last_p) {
                ups++;
            }
            last_p = b.percent;
            if (b.bars != last_b) {
                printf("  %5.1f min: %d bars (true %.0f %%, shown %d %%)\n", (double)t / 60.0, b.bars, (double)soc,
                       b.percent);
                last_b = b.bars;
            }
            if (b.low != last_low) {
                printf("  %5.1f min: low battery %s (true %.0f %%, shown %d %%)\n", (double)t / 60.0,
                       b.low ? "on" : "off", (double)soc, b.percent);
                last_low = b.low;
            }
        }
        printf("  shown percent went up %d times  %s\n", ups, ups ? "FAIL" : "ok");
        s_fail += ups != 0;
    }

    printf("charging (threshold %d mV, hysteresis %d mV):\n", BAT_CHARGING_MV, BAT_CHARGING_HYST_MV);
    {
        batt_t b;
        batt_init(&b);
        for (int t = 0; t < 60; t++) {
            batt_update(&b, 3900.0f + noise(40.0f), 1.0f);
        }
        int on_at = -1, off_at = -1, flips = 0;
        bool last = b.charging;
        for (int t = 0; t < 300; t++) {
            const float v = t < 120 ? 4220.0f : 4150.0f; // plugged in (cell full) for 2 min, then unplugged
            batt_update(&b, v + noise(40.0f), 1.0f);
            if (b.charging != last) {
                flips++;
                if (b.charging && on_at < 0) {
                    on_at = t;
                }
                if (!b.charging && t >= 120 && off_at < 0) {
                    off_at = t - 120;
                }
                last = b.charging;
            }
        }
        const bool ok = on_at >= 0 && off_at >= 0 && flips == 2;
        printf("  3.90 V -> 4.22 V: charging after %d s; 4.22 -> 4.15 V: not charging after %d s; %d changes  %s\n",
               on_at, off_at, flips, ok ? "ok" : "FAIL");
        s_fail += !ok;
    }
    printf("cutoff (%d mV, +%d mV to resume after a cutoff, enabled %d):\n", BAT_CUTOFF_MV, BAT_CUTOFF_RESUME_MV,
           BAT_CUTOFF_ENABLE);
    {
        const struct {
            float mv;
            bool after;
            bool want;
        } c[] = { { 3250, false, true }, { 3299, false, true }, { 3300, false, false }, { 3350, false, false },
                  { 3350, true, true },  { 3399, true, true },  { 3400, true, false } };
        for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
            const bool got = batt_below_cutoff(c[i].mv, c[i].after);
            const bool ok = got == (c[i].want && BAT_CUTOFF_ENABLE);
            printf("  %4.0f mV%s: %s  %s\n", (double)c[i].mv, c[i].after ? " after a cutoff" : "",
                   got ? "below (sleep)" : "run", ok ? "ok" : "FAIL");
            s_fail += !ok;
        }
    }
    printf("drain estimate:\n");
    {
        const float d1 = batt_drain_mv_per_day(3950.0f, 3940.0f, 2.0 * 86400.0);
        const float d2 = batt_drain_mv_per_day(4000.0f, 3880.0f, 12.0 * 3600.0);
        const bool ok = fabsf(d1 - 5.0f) < 0.01f && fabsf(d2 - 240.0f) < 0.01f;
        printf("  3950 -> 3940 mV in 2 days: %.1f mV/day; 4000 -> 3880 mV in 12 h: %.1f mV/day  %s\n", (double)d1,
               (double)d2, ok ? "ok" : "FAIL");
        s_fail += !ok;
    }
    printf("%s\n", s_fail ? "BATTERY FAILURES" : "all battery checks ok");
    return s_fail != 0;
}
