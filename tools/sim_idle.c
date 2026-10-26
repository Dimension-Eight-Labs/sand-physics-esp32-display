// Host test: idle dimming (src/idle.c) against the hand-motion model (no
// ESP-IDF needed).
//
//   mkdir -p build-host && cc -std=c11 -O2 -Isrc src/sim.c src/material.c src/idle.c tools/sim_idle.c -lm
//       -o build-host/sim_idle && build-host/sim_idle
//
// Per material: a settled pile held in the hand (tools/hand_model.h) for
// 10 minutes, fed through the detector the way the sim task does (filtered
// gravity, shake flag, sim motion). Hand tremor and sway must never count as
// activity, so the screen dims IDLE_DIM_AFTER_S after the last real one. Then
// a deliberate tilt and a knock must each restore full brightness at once.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "idle.h"
#include "material.h"
#include "sim.h"

#define DEG (3.14159265f / 180.0f)
#define HAND_LEAN 20.0f
#include "hand_model.h"

static sim_t sim;

static int64_t step_us(int64_t step)
{
    return step * 1000000 / SIM_HZ;
}

static void run(int material)
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
    hand_t h = { .seed = 3 };
    h.lp[1] = cosf(HAND_LEAN * DEG);
    h.lp[2] = -sinf(HAND_LEAN * DEG);

    idle_t d;
    int64_t step = 0;
    idle_init(&d, 0);
    float g[3];
    // The hand picks the board up: one real activity at t = 0, then 10 min.
    hand_step(&h, g);
    idle_update(&d, g, true, 0.0f, 0);
    int events = 0;
    int64_t last_event = 0;
    float motion_max = 0.0f, tilt_max = 0.0f;
    const float ref[3] = { g[0], g[1], g[2] };
    const float rn = sqrtf(ref[0] * ref[0] + ref[1] * ref[1] + ref[2] * ref[2]);
    int64_t dim_start = -1, dim_full = -1;
    for (step = 1; step <= (int64_t)SIM_HZ * 600; step++) {
        hand_step(&h, g);
        sim_set_gravity(&sim, g[0], g[1], g[2]);
        sim_step(&sim);
        const int64_t t = step_us(step);
        if (idle_update(&d, g, false, sim.motion, t)) {
            events++;
            last_event = t;
        }
        motion_max = sim.motion > motion_max ? sim.motion : motion_max;
        const float gn = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        const float c = (g[0] * ref[0] + g[1] * ref[1] + g[2] * ref[2]) / (gn * rn);
        const float a = acosf(c > 1.0f ? 1.0f : c) / DEG;
        tilt_max = a > tilt_max ? a : tilt_max;
        const float dim = idle_dim_amount(t - d.last_us);
        if (dim > 0.0f && dim_start < 0) {
            dim_start = t;
        }
        if (dim >= 1.0f && dim_full < 0) {
            dim_full = t;
        }
    }
    printf("%-6s held in hand 10 min: %d activity events after pick-up (last at %.1f s); hand moved the board up "
           "to %.1f deg from where it was picked up (wake tilt %.0f); sim motion up to %.1f (wake %.0f); dimming "
           "starts at %.1f s, fully dim at %.1f s\n",
           m->name, events, (double)last_event / 1e6, (double)tilt_max, (double)IDLE_WAKE_TILT_DEG,
           (double)motion_max, (double)IDLE_WAKE_MOTION, dim_start < 0 ? -1.0 : (double)dim_start / 1e6,
           dim_full < 0 ? -1.0 : (double)dim_full / 1e6);

    // A deliberate tilt on top of the hand motion: 30 degrees over 0.3 s.
    int64_t woke = -1;
    const int64_t t0 = step_us(step);
    for (int i = 0; i < SIM_HZ && woke < 0; i++, step++) {
        h.extra_rot = 30.0f * fminf(1.0f, (float)i / (0.3f * SIM_HZ));
        hand_step(&h, g);
        sim_set_gravity(&sim, g[0], g[1], g[2]);
        sim_step(&sim);
        if (idle_update(&d, g, false, sim.motion, step_us(step))) {
            woke = step_us(step) - t0;
        }
    }
    const float after_tilt = idle_dim_amount(step_us(step) - d.last_us);
    // Leave it dim again, then a knock.
    d.last_us = step_us(step) - (int64_t)(IDLE_DIM_AFTER_S + 10) * 1000000;
    const float before_knock = idle_dim_amount(step_us(step) - d.last_us);
    hand_step(&h, g);
    idle_update(&d, g, true, sim.motion, step_us(step));
    printf("       deliberate 30 deg tilt while dimmed: activity after %.0f ms (dim then %.2f); knock while dimmed "
           "(dim %.2f): dim then %.2f\n",
           woke < 0 ? -1.0 : (double)woke / 1000.0, (double)after_tilt, (double)before_knock,
           (double)idle_dim_amount(step_us(step) - d.last_us));
    free(mem);
}

int main(void)
{
    printf("idle dimming: after %d s, fade %d ms, backlight %d %% -> %d %%, LED x%.2f; wake on knock, tilt > %.0f deg, "
           "motion > %.0f\n",
           IDLE_DIM_AFTER_S, IDLE_DIM_FADE_MS, BACKLIGHT_PERCENT, IDLE_DIM_BACKLIGHT_PERCENT,
           (double)IDLE_DIM_LED_SCALE, (double)IDLE_WAKE_TILT_DEG, (double)IDLE_WAKE_MOTION);
    printf("dim curve: %.2f at %d s idle, %.2f at +1 s, %.2f at +2 s\n", (double)idle_dim_amount((int64_t)IDLE_DIM_AFTER_S * 1000000),
           IDLE_DIM_AFTER_S, (double)idle_dim_amount((int64_t)(IDLE_DIM_AFTER_S + 1) * 1000000),
           (double)idle_dim_amount((int64_t)(IDLE_DIM_AFTER_S + 2) * 1000000));
    run(MATERIAL_CLASSIC);
    run(MATERIAL_SAND);
    run(MATERIAL_LIQUID);
    return 0;
}
