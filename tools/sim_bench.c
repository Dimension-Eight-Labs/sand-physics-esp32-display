// Step-cost benchmark for the simulation, per material (no ESP-IDF needed).
//
//   cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_bench.c -lm -o sim_bench && ./sim_bench
//
// Scenarios (86x160 grid, FILL_FRACTION grains):
//   settled  at rest on the floor, board upright
//   freefall the first 60 steps after a respawn: every grain falling
//   shake    a settled bed shaken every other step
//   flow     a settled bed turned 90 degrees: a large avalanche / pour
//   slosh    the board rocked +/-45 degrees every 0.5 s
//   steer    the board rotated 1 degree per step: nothing can sleep
//   awake    a settled bed with every grain woken each step but not pushed
//            (on the board this resting-but-awake case is the most expensive)
// Prints host mean and worst microseconds per step (best of 5 runs) and the
// average number of grains moving per step.
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "sim.h"
#ifndef V1_API
#include "material.h"
#include "overrides.h"
#endif

#define DEG (3.14159265f / 180.0f)

static sim_t sim;
static void *mem;
#ifndef V1_API
static material_t mats[MATERIAL_COUNT]; // with optional liquid overrides (key=value)
#endif

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void fresh(int material)
{
    int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 4242);
#ifndef V1_API // build against the pre-material code for comparison
    sim_set_material(&sim, &mats[material]);
#else
    (void)material;
#endif
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    sim_set_gravity(&sim, 0, 1, 0);
}

static void rotate(float deg)
{
    sim_set_gravity(&sim, sinf(deg * DEG), cosf(deg * DEG), 0);
}

static int moving_grains(void)
{
    int m = 0;
    for (int i = 0; i < sim.n; i++) {
        m += sim.grains[i].vx != 0.0f || sim.grains[i].vy != 0.0f;
    }
    return m;
}

typedef enum { SC_SETTLED, SC_FREEFALL, SC_SHAKE, SC_FLOW, SC_SLOSH, SC_STEER, SC_AWAKE } scenario_t;
static const char *NAMES[] = { "settled", "freefall", "shake", "flow", "slosh", "steer", "awake" };

static void bench(int material, scenario_t sc, int steps)
{
    double best_mean = 1e30, best_max = 1e30;
    long moved = 0;
    for (int rep = 0; rep < 5; rep++) {
        fresh(material);
        if (sc != SC_FREEFALL) {
            for (int i = 0; i < 2500; i++) {
                sim_step(&sim);
            }
        }
        if (sc == SC_FLOW) {
            rotate(90);
        }
        double total = 0, worst = 0;
        moved = 0;
        for (int i = 0; i < steps; i++) {
            if (sc == SC_SHAKE && (i & 1) == 0) {
                sim_shake(&sim, 0.0f, -1.0f, SHAKE_STRENGTH);
            }
            if (sc == SC_SLOSH && i % (SIM_HZ / 2) == 0) {
                rotate((i / (SIM_HZ / 2)) % 2 ? -45.0f : 45.0f);
            }
            if (sc == SC_STEER) {
                rotate((float)i);
            }
            if (sc == SC_AWAKE) {
                sim_shake(&sim, 0.0f, 0.0f, 0.0f); // wakes everyone, adds no velocity
            }
            double t0 = now_us();
            sim_step(&sim);
            double dt = now_us() - t0;
            total += dt;
            worst = dt > worst ? dt : worst;
            moved += moving_grains();
        }
        best_mean = total / steps < best_mean ? total / steps : best_mean;
        best_max = worst < best_max ? worst : best_max;
    }
    printf("  %-8s %7.1f us mean %7.1f us worst   %5ld of %d moving\n", NAMES[sc], best_mean, best_max,
           moved / steps, sim.n);
}

int main(int argc, char **argv)
{
#ifndef V1_API
    for (int m = 0; m < MATERIAL_COUNT; m++) {
        mats[m] = *material_get(m);
    }
    // name=value arguments override liquid parameters (tools/overrides.h),
    // e.g. viscosity=0.3 knock_splash=0
    for (int a = 1; a < argc; a++) {
        override_apply(&mats[MATERIAL_LIQUID], argv[a]);
    }
    override_print(material_get(MATERIAL_LIQUID), &mats[MATERIAL_LIQUID]);
#else
    (void)argc;
    (void)argv;
#endif
#ifdef V1_API
    for (int m = 0; m < 1; m++) {
        printf("classic physics v1 (reference build)\n");
#else
    for (int m = 0; m < MATERIAL_COUNT; m++) {
        printf("%s\n", material_get(m)->name);
#endif
        bench(m, SC_SETTLED, 600);
        bench(m, SC_FREEFALL, 60);
        bench(m, SC_SHAKE, 600);
        bench(m, SC_FLOW, 600);
        bench(m, SC_SLOSH, 600);
        bench(m, SC_STEER, 600);
        bench(m, SC_AWAKE, 600);
    }
    return 0;
}
