// Battery level; see batt.h.
#include "batt.h"

#include "config.h"

// Typical single-cell LiPo open-circuit voltage vs. state of charge, light
// load, 5 % steps (a common published table; real cells vary by a few %).
static const struct {
    float mv;
    float pct;
} CURVE[] = {
    { 3270, 0 },   { 3610, 5 },   { 3690, 10 },  { 3710, 15 },  { 3730, 20 },  { 3750, 25 },  { 3770, 30 },
    { 3790, 35 },  { 3800, 40 },  { 3820, 45 },  { 3840, 50 },  { 3850, 55 },  { 3870, 60 },  { 3910, 65 },
    { 3950, 70 },  { 3980, 75 },  { 4020, 80 },  { 4080, 85 },  { 4110, 90 },  { 4150, 95 },  { 4200, 100 },
};
#define CURVE_N ((int)(sizeof(CURVE) / sizeof(CURVE[0])))

float batt_percent_from_mv(float mv)
{
    if (mv <= CURVE[0].mv) {
        return 0.0f;
    }
    for (int i = 1; i < CURVE_N; i++) {
        if (mv < CURVE[i].mv) {
            const float t = (mv - CURVE[i - 1].mv) / (CURVE[i].mv - CURVE[i - 1].mv);
            return CURVE[i - 1].pct + t * (CURVE[i].pct - CURVE[i - 1].pct);
        }
    }
    return 100.0f;
}

void batt_init(batt_t *b)
{
    *b = (batt_t){ .percent = -1, .bars = -1 };
}

// Bars: n bars from n * 20 % (rounded: 1 bar above 10 %, ... 5 above 90 %),
// changing only once the percentage is BAT_HYST_PERCENT past the boundary.
static int bars_for(float pct, int current)
{
    const int raw = pct >= 90.0f ? 5 : pct >= 70.0f ? 4 : pct >= 50.0f ? 3 : pct >= 30.0f ? 2 : pct >= 10.0f ? 1 : 0;
    if (current < 0 || raw == current) {
        return raw;
    }
    // Boundary between `current` and the neighbour toward `raw`.
    const float edge = raw > current ? 10.0f + 20.0f * (float)current : 10.0f + 20.0f * (float)(current - 1);
    if (raw > current ? pct >= edge + BAT_HYST_PERCENT : pct < edge - BAT_HYST_PERCENT) {
        return raw;
    }
    return current;
}

void batt_update(batt_t *b, float cell_mv, float dt_s)
{
    if (cell_mv < BAT_VALID_MIN_MV) {
        return; // not a battery reading; keep the last good state
    }
    // One-pole low-pass, time constant BAT_FILTER_S, started as a running
    // mean of the first readings so one noisy first sample does not stick.
    // A faster one (BAT_FAST_FILTER_S) only for the charging hint.
    b->n++;
    if (!b->valid) {
        b->mv = b->mv_fast = cell_mv;
        b->valid = true;
    } else {
        float k = dt_s / (BAT_FILTER_S + dt_s);
        if (k < 1.0f / (float)b->n) {
            k = 1.0f / (float)b->n;
        }
        b->mv += k * (cell_mv - b->mv);
        b->mv_fast += dt_s / (BAT_FAST_FILTER_S + dt_s) * (cell_mv - b->mv_fast);
    }
    // Charging (on USB): above the threshold; ends BAT_CHARGING_HYST_MV below.
    b->charging = b->charging ? b->mv_fast > BAT_CHARGING_MV - BAT_CHARGING_HYST_MV : b->mv_fast > BAT_CHARGING_MV;

    // Until the filter has settled the shown percent follows it both ways.
    const bool settling = (float)b->n * dt_s < BAT_FILTER_S;
    // Shown percent: on battery the charge only goes down, so it follows the
    // reading down once it is BAT_HYST_PERCENT below, but needs
    // BAT_RISE_PERCENT to go up (charging, or a recovering cell after the
    // backlight dims, which is not more charge). Noise cannot walk it.
    const float pct = batt_percent_from_mv(b->mv);
    if (b->percent < 0 || pct <= (float)(b->percent - BAT_HYST_PERCENT) ||
        pct >= (float)(b->percent + (settling ? BAT_HYST_PERCENT : BAT_RISE_PERCENT))) {
        b->percent = (int)(pct + 0.5f);
    }
    b->bars = bars_for((float)b->percent, b->bars);

#if BAT_LOW_ENABLE
    if (b->charging) {
        b->low = false;
    } else if (b->low) {
        b->low = b->percent < BAT_LOW_PERCENT + BAT_HYST_PERCENT;
    } else {
        b->low = b->percent < BAT_LOW_PERCENT;
    }
#else
    b->low = false;
#endif
}

bool batt_below_cutoff(float mv, bool after_cutoff)
{
#if BAT_CUTOFF_ENABLE
    return mv < (float)(BAT_CUTOFF_MV + (after_cutoff ? BAT_CUTOFF_RESUME_MV : 0));
#else
    (void)mv;
    (void)after_cutoff;
    return false;
#endif
}

float batt_drain_mv_per_day(float mv_then, float mv_now, double seconds)
{
    return seconds > 0.0 ? (float)((double)(mv_then - mv_now) * 86400.0 / seconds) : 0.0f;
}
