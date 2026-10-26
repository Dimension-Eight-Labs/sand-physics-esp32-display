// Host test: a settled pool / pile held in the hand (no ESP-IDF needed).
//
//   cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_hand.c -lm -o sim_hand && ./sim_hand
//
// The board is held leaned back HAND_LEAN degrees with hand motion: +/-0.03 g
// white tremor on every axis at 200 Hz, an in-plane sway of +/-3 degrees at
// 0.25 Hz plus a slow +/-2 degree drift at 0.07 Hz, and a +/-2 degree lean
// wobble at 0.3 Hz, all through the firmware's gravity low-pass. Prints, per
// second: awake grains (mean / max), host step time (mean / max), wake-all
// events, levelling transfers, spread pushes and motion. Then a deliberate
// 20 degree tilt checks that the material still reacts at once.
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "material.h"
#include "sim.h"
#include "overrides.h"

#define DEG (3.14159265f / 180.0f)
#define HAND_LEAN 20.0f

static sim_t sim;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static float urand(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (float)((*seed >> 8) & 0xFFFF) / 32767.5f - 1.0f;
}

typedef struct {
    float lp[3];
    double t;
    unsigned seed;
    float extra_rot; // deliberate rotation added on top of the hand motion
} hand_t;

// Advance the simulated IMU by one sim step and return filtered gravity.
static void hand_step(hand_t *h, float out[3])
{
    const double imu_dt = 1.0 / IMU_POLL_HZ;
    double end = h->t + 1.0 / SIM_HZ;
    while (h->t < end) {
        float t = (float)h->t;
        float rot = 3.0f * sinf(t * 2.0f * 3.14159265f * 0.25f) + 2.0f * sinf(t * 2.0f * 3.14159265f * 0.07f) +
                    h->extra_rot;
        float lean = HAND_LEAN + 2.0f * sinf(t * 2.0f * 3.14159265f * 0.3f);
        float in_plane = cosf(lean * DEG);
        float g[3] = {
            in_plane * sinf(rot * DEG) + 0.03f * urand(&h->seed),
            in_plane * cosf(rot * DEG) + 0.03f * urand(&h->seed),
            -sinf(lean * DEG) + 0.03f * urand(&h->seed),
        };
        for (int i = 0; i < 3; i++) {
            h->lp[i] += IMU_LPF_ALPHA * (g[i] - h->lp[i]);
        }
        h->t += imu_dt;
    }
    memcpy(out, h->lp, sizeof(h->lp));
}

static void run_hand(const material_t *m)
{
    int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    void *mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 77);
    sim_set_material(&sim, m);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    hand_t h = { .lp = { 0, 1, 0 }, .seed = 3 };
    float g[3];
    // Settle while held still at the hand's mean attitude.
    for (int i = 0; i < 2500; i++) {
        sim_set_gravity(&sim, 0, cosf(HAND_LEAN * DEG), -sinf(HAND_LEAN * DEG));
        sim_step(&sim);
    }
    h.lp[0] = 0;
    h.lp[1] = cosf(HAND_LEAN * DEG);
    h.lp[2] = -sinf(HAND_LEAN * DEG);

    printf("%s, held in hand (lean %.0f deg, tremor +/-0.03 g, sway +/-3+2 deg, lean wobble +/-2 deg):\n",
           m->name, (double)HAND_LEAN);
    printf("   s  awake mean/max   host us/step mean/max  wake-alls transfers pushes  motion\n");
    double total_us = 0;
    long total_awake = 0;
    int total_steps = 0;
    for (int sec = 1; sec <= 20; sec++) {
        long awake = 0;
        int awake_max = 0;
        double us = 0, us_max = 0, motion = 0;
        uint32_t w0 = sim.n_wake_all, t0 = sim.n_transfers, p0 = sim.n_pushes;
        for (int i = 0; i < SIM_HZ; i++) {
            hand_step(&h, g);
            double a = now_us();
            sim_set_gravity(&sim, g[0], g[1], g[2]);
            sim_step(&sim);
            double d = now_us() - a;
            us += d;
            us_max = d > us_max ? d : us_max;
            awake += sim.awake;
            awake_max = sim.awake > awake_max ? sim.awake : awake_max;
            motion += (double)sim.motion;
        }
        total_us += us;
        total_awake += awake;
        total_steps += SIM_HZ;
        if (sec <= 6 || sec % 5 == 0) {
            printf("  %2d  %5ld / %4d     %6.1f / %6.1f       %4u     %5u   %5u   %6.1f\n", sec, awake / SIM_HZ,
                   awake_max, us / SIM_HZ, us_max, sim.n_wake_all - w0, sim.n_transfers - t0, sim.n_pushes - p0,
                   motion / SIM_HZ);
        }
    }
    printf("  20 s average: %ld awake, %.1f us/step on host\n", total_awake / total_steps, total_us / total_steps);

    // A deliberate tilt on top of the hand motion: 20 degrees over 0.2 s.
    // Reaction = more than 20 grains changing cell in one step.
    int react = -1, moved_1s = 0;
    uint32_t tr0 = sim.n_transfers, pu0 = sim.n_pushes;
    for (int i = 0; i < SIM_HZ; i++) {
        h.extra_rot = 20.0f * fminf(1.0f, (float)i / (0.2f * SIM_HZ));
        hand_step(&h, g);
        uint32_t before = sim.moves;
        sim_set_gravity(&sim, g[0], g[1], g[2]);
        sim_step(&sim);
        moved_1s += (int)(sim.moves - before);
        if (react < 0 && sim.moves - before > 20) {
            react = i;
        }
    }
    if (react >= 0) {
        printf("  deliberate 20 deg tilt over 0.2 s: >20 grains move per step after %d steps (%.0f ms); "
               "%d cell moves in 1 s (%u transfers, %u pushes)\n\n", react, react * 1000.0 / SIM_HZ, moved_1s,
               sim.n_transfers - tr0, sim.n_pushes - pu0);
    } else {
        printf("  deliberate 20 deg tilt over 0.2 s: no flow (%d cell moves in 1 s, %u transfers, %u pushes)%s\n\n",
               moved_1s, sim.n_transfers - tr0, sim.n_pushes - pu0,
               m->rest_rule == MAT_REST_REPOSE ? " - expected for sand: 20 deg is below the angle of repose" : "");
    }
    free(mem);
}

int main(int argc, char **argv)
{
    // name=value arguments override liquid parameters (tools/overrides.h).
    static material_t liquid;
    liquid = *material_get(MATERIAL_LIQUID);
    for (int a = 1; a < argc; a++) {
        override_apply(&liquid, argv[a]);
    }
    override_print(material_get(MATERIAL_LIQUID), &liquid);
    run_hand(material_get(MATERIAL_CLASSIC));
    run_hand(&liquid);
    return 0;
}
