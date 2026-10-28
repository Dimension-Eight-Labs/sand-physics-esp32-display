// Host test: BOOT button gestures (src/gesture.c) and auto-off (src/idle.c)
// (no ESP-IDF needed).
//
//   mkdir -p build-host && cc -std=c11 -O2 -Isrc -Itools src/sim.c src/material.c src/idle.c src/gesture.c
//       tools/sim_power.c -lm -o build-host/sim_power && build-host/sim_power
//
// Gestures: the raw button is sampled every GESTURE_POLL_MS, as the firmware
// does, with contact bounce on every edge. Each scripted sequence must give
// exactly the expected events, at the expected times; then press length and
// the gap between two presses are swept to show where one gesture ends and
// the next begins.
//
// Auto-off: a settled pile held in the hand (tools/hand_model.h) must power
// off AUTO_OFF_S after the pick-up (hand tremor is not activity); with a
// deliberate tilt every 5 minutes for 10 minutes it must stay on, and power
// off AUTO_OFF_S after the last one.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "gesture.h"
#include "idle.h"
#include "material.h"
#include "sim.h"

#define DEG (3.14159265f / 180.0f)
#define HAND_LEAN 20.0f
#include "hand_model.h"

// ---- Gestures -------------------------------------------------------------------

#define MAX_EV 8

typedef struct {
    uint32_t ev[MAX_EV];
    uint32_t t[MAX_EV]; // ms
    int n;
    uint32_t bar_first, bar_last; // ms when the progress bar showed (0 = never)
} result_t;

static uint32_t s_rng = 12345;
static bool s_bounce = true;

static int rnd(int n)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (int)((s_rng >> 16) % (uint32_t)n);
}

// segs: alternating up/down durations in ms, starting with `start_down`.
// Bounce: for 0..2 polls after each edge the raw level reads randomly.
static result_t run_seq(bool start_down, const int *segs, int nseg)
{
    result_t r = { 0 };
    gesture_t g;
    gesture_init(&g, start_down, 0);
    uint32_t t = 0;
    bool level = start_down;
    for (int s = 0; s <= nseg; s++) {
        const int len = s < nseg ? segs[s] : 1000; // trailing idle second
        const int bounce = s_bounce ? rnd(3) : 0;
        for (int k = 0; k < len; k += GESTURE_POLL_MS) {
            t += GESTURE_POLL_MS;
            bool raw = level;
            if (k / GESTURE_POLL_MS < bounce && s > 0) {
                raw = rnd(2);
            }
            uint32_t ev = gesture_update(&g, raw, t);
            for (int b = 0; b < 4; b++) {
                if ((ev >> b) & 1u && r.n < MAX_EV) {
                    r.ev[r.n] = 1u << b;
                    r.t[r.n++] = t;
                }
            }
            if (gesture_hold_progress(&g, t) >= 0.0f) {
                if (!r.bar_first) {
                    r.bar_first = t;
                }
                r.bar_last = t;
            }
        }
        level = !level;
    }
    return r;
}

static const char *ev_name(uint32_t e)
{
    switch (e) {
    case GESTURE_CLICK: return "click";
    case GESTURE_DOUBLE: return "double";
    case GESTURE_LONG: return "long";
    case GESTURE_POWER_OFF: return "power-off";
    default: return "?";
    }
}

static void fmt(const result_t *r, char *buf, size_t n)
{
    buf[0] = 0;
    if (!r->n) {
        snprintf(buf, n, "nothing");
        return;
    }
    for (int i = 0; i < r->n; i++) {
        size_t l = strlen(buf);
        snprintf(buf + l, n - l, "%s%s", i ? "+" : "", ev_name(r->ev[i]));
    }
}

static int s_fail;

static void check(const char *name, bool start_down, const int *segs, int nseg, const char *want)
{
    char got[96];
    // Several bounce patterns per scenario: the result must not depend on them.
    result_t r0 = run_seq(start_down, segs, nseg);
    fmt(&r0, got, sizeof(got));
    bool ok = strcmp(got, want) == 0;
    for (int rep = 0; rep < 20 && ok; rep++) {
        char again[96];
        result_t r = run_seq(start_down, segs, nseg);
        fmt(&r, again, sizeof(again));
        ok = strcmp(again, got) == 0;
    }
    // Event times relative to the last release in the script.
    uint32_t t_end = 0;
    for (int i = 0; i < nseg; i++) {
        t_end += (uint32_t)segs[i];
    }
    char when[128] = "";
    for (int i = 0; i < r0.n; i++) {
        size_t l = strlen(when);
        snprintf(when + l, sizeof(when) - l, "%s%s at %u ms", i ? ", " : "", ev_name(r0.ev[i]), r0.t[i]);
    }
    char bar[64] = "";
    if (r0.bar_first) {
        snprintf(bar, sizeof(bar), "; power-off bar %u..%u ms", r0.bar_first, r0.bar_last);
    }
    printf("  %-44s -> %-14s %s  (%s%s; script ends %u ms)\n", name, got, ok ? "ok" : "FAIL", r0.n ? when : "-", bar,
           t_end);
    s_fail += !ok;
}

static void gestures(void)
{
    printf("gestures: double-click window %d ms, long press %d ms, power-off bar from %d ms, power off at %d ms; "
           "polled every %d ms with 0-2 polls of contact bounce per edge\n",
           BTN_DOUBLE_MS, BTN_LONG_MS, BTN_HOLD_SHOW_MS, BTN_POWER_OFF_MS, GESTURE_POLL_MS);
    // Segments alternate up/down; scripts start released.
    check("click (120 ms)", false, (int[]){ 200, 120 }, 2, "click");
    check("double (100 down, 150 up, 100 down)", false, (int[]){ 200, 100, 150, 100 }, 4, "double");
    check("double, slow (200 down, 280 up, 200 down)", false, (int[]){ 200, 200, 280, 200 }, 4, "double");
    check("two clicks 400 ms apart", false, (int[]){ 200, 100, 400, 100 }, 4, "click+click");
    check("triple click", false, (int[]){ 200, 100, 150, 100, 150, 100 }, 6, "double+click");
    check("long press (1200 ms)", false, (int[]){ 200, 1200 }, 2, "long");
    check("long press (1450 ms)", false, (int[]){ 200, 1450 }, 2, "long");
    check("released with the bar showing (1600 ms)", false, (int[]){ 200, 1600 }, 2, "nothing");
    check("released with the bar showing (2900 ms)", false, (int[]){ 200, 2900 }, 2, "nothing");
    check("hold (3500 ms)", false, (int[]){ 200, 3500 }, 2, "power-off");
    check("hold (8000 ms)", false, (int[]){ 200, 8000 }, 2, "power-off");
    check("click, then long press 150 ms later", false, (int[]){ 200, 100, 150, 1200 }, 4, "long");
    check("click, then hold 150 ms later", false, (int[]){ 200, 100, 150, 3500 }, 4, "power-off");
    check("long press, then click", false, (int[]){ 200, 1200, 200, 100 }, 4, "long+click");
    check("held at boot (2 s), released", true, (int[]){ 2000 }, 1, "nothing");
    check("held at boot (5 s), released", true, (int[]){ 5000 }, 1, "nothing");
    check("held at boot, released, click", true, (int[]){ 2000, 300, 100 }, 3, "click");
    // Bounce could stretch a 20 ms blip to 40 ms; these two run without it.
    s_bounce = false;
    check("20 ms blip (no bounce)", false, (int[]){ 200, 20 }, 2, "nothing");
    check("click, 20 ms blip in the window (no bounce)", false, (int[]){ 200, 100, 100, 20 }, 4, "click");
    s_bounce = true;

    // Press length sweep: which gesture each length gives.
    printf("  press length sweep (10 ms steps, single press):\n");
    char prev[96] = "";
    int from = 0;
    for (int len = 10; len <= 4000 + 10; len += 10) {
        char got[96];
        result_t r = run_seq(false, (int[]){ 200, len }, 2);
        fmt(&r, got, sizeof(got));
        if (len > 4000 || strcmp(got, prev) != 0) {
            if (prev[0]) {
                printf("    %4d..%4d ms: %s\n", from, len - 10, prev);
            }
            snprintf(prev, sizeof(prev), "%s", got);
            from = len;
        }
    }
    // Gap sweep: two 100 ms presses.
    printf("  gap sweep (two 100 ms presses):\n");
    prev[0] = 0;
    for (int gap = 30; gap <= 600 + 10; gap += 10) {
        char got[96];
        result_t r = run_seq(false, (int[]){ 200, 100, gap, 100 }, 4);
        fmt(&r, got, sizeof(got));
        if (gap > 600 || strcmp(got, prev) != 0) {
            if (prev[0]) {
                printf("    gap %3d..%3d ms: %s\n", from, gap - 10, prev);
            }
            snprintf(prev, sizeof(prev), "%s", got);
            from = gap;
        }
    }
}

// ---- Auto-off --------------------------------------------------------------------

static sim_t sim;

static void auto_off(int material, int tilt_every_s)
{
    const material_t *m = material_get(material);
    const int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    void *mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 77);
    sim_set_material(&sim, m);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    for (int i = 0; i < 2500; i++) {
        sim_set_gravity(&sim, 0, cosf(HAND_LEAN * DEG), -sinf(HAND_LEAN * DEG));
        sim_step(&sim);
    }
    hand_t h = { .seed = 5 };
    h.lp[1] = cosf(HAND_LEAN * DEG);
    h.lp[2] = -sinf(HAND_LEAN * DEG);

    idle_t d;
    idle_init(&d, 0);
    float g[3];
    hand_step(&h, g);
    idle_update(&d, g, true, 0.0f, 0); // the pick-up
    int64_t off_at = -1, last_tilt = 0;
    int tilts = 0;
    const int64_t total = (int64_t)SIM_HZ * 30 * 60;
    for (int64_t step = 1; step <= total && off_at < 0; step++) {
        const int64_t t = step * 1000000 / SIM_HZ;
        // A deliberate 40 degree tilt over 0.5 s, back over the next 0.5 s.
        if (tilt_every_s > 0 && t < (10LL * 60 + 1) * 1000000) {
            const int64_t ph = t % ((int64_t)tilt_every_s * 1000000);
            const float x = (float)ph / 1e6f;
            h.extra_rot = x < 0.5f ? 80.0f * x : x < 1.0f ? 80.0f * (1.0f - x) : 0.0f;
        } else {
            h.extra_rot = 0.0f;
        }
        hand_step(&h, g);
        sim_set_gravity(&sim, g[0], g[1], g[2]);
        sim_step(&sim);
        if (idle_update(&d, g, false, sim.motion, t)) {
            if (t - last_tilt > 2000000) {
                tilts++;
            }
            last_tilt = t;
        }
        if (idle_auto_off(t - d.last_us)) {
            off_at = t;
        }
    }
    if (tilt_every_s > 0) {
        printf("  %-6s held, tilted every %d s for 10 min: %d activity bursts, last activity %.1f s, "
               "auto-off at %.1f s (%.1f s after it)\n",
               m->name, tilt_every_s, tilts, (double)last_tilt / 1e6, (double)off_at / 1e6,
               (double)(off_at - last_tilt) / 1e6);
    } else {
        printf("  %-6s held in hand: %d activity bursts after the pick-up, auto-off at %.1f s\n", m->name, tilts,
               (double)off_at / 1e6);
    }
    free(mem);
}

int main(void)
{
    gestures();
    printf("auto-off: enabled %d, after %d s without activity (activity as for idle dimming)\n", AUTO_OFF_ENABLE,
           AUTO_OFF_S);
    const int mats[3] = { MATERIAL_CLASSIC, MATERIAL_SAND, MATERIAL_LIQUID };
    for (int i = 0; i < 3; i++) {
        auto_off(mats[i], 0);
        auto_off(mats[i], 300);
    }
    printf("%s\n", s_fail ? "GESTURE FAILURES" : "all gesture checks ok");
    return s_fail != 0;
}
