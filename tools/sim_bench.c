// Step-cost benchmark for the sand simulation (no ESP-IDF needed).
//
//   cc -std=c11 -O2 -Isrc src/sim.c tools/sim_bench.c -lm -o sim_bench && ./sim_bench
//
// Scenarios (86x160 grid, FILL_FRACTION grains):
//   settled  sand at rest on the floor, board upright
//   freefall the first 60 steps after a respawn: every grain falling
//   shake    a settled bed shaken every other step (the worst case)
//   flow     a settled bed turned 90 degrees: a large avalanche
// Prints host microseconds per step and how many grains moved per step.
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "sim.h"

static sim_t sim;
static void *mem;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void fresh(void)
{
    int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 4242);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    sim_set_gravity(&sim, 0, 1, 0);
}

static void settle(void)
{
    for (int i = 0; i < 2500; i++) {
        sim_step(&sim);
    }
}

static int moving_grains(void)
{
    int m = 0;
    for (int i = 0; i < sim.n; i++) {
        m += sim.grains[i].vx != 0.0f || sim.grains[i].vy != 0.0f;
    }
    return m;
}

typedef enum { SC_SETTLED, SC_FREEFALL, SC_SHAKE, SC_FLOW } scenario_t;

static void bench(const char *name, scenario_t sc, int steps)
{
    double best = 1e30;
    long moved = 0;
    for (int rep = 0; rep < 5; rep++) {
        fresh();
        if (sc != SC_FREEFALL) {
            settle();
        }
        if (sc == SC_FLOW) {
            sim_set_gravity(&sim, 1, 0, 0);
        }
        double t = 0;
        moved = 0;
        for (int i = 0; i < steps; i++) {
            if (sc == SC_SHAKE && (i & 1) == 0) {
                sim_shake(&sim, 0.0f, -1.0f, SHAKE_STRENGTH);
            }
            double t0 = now_us();
            sim_step(&sim);
            t += now_us() - t0;
            moved += moving_grains();
        }
        if (t / steps < best) {
            best = t / steps;
        }
    }
    printf("%-9s %7.1f us/step   %5ld of %d grains moving per step\n", name, best, moved / steps, sim.n);
}

int main(void)
{
    bench("settled", SC_SETTLED, 600);
    bench("freefall", SC_FREEFALL, 60);
    bench("shake", SC_SHAKE, 600);
    bench("flow", SC_FLOW, 600);
    return 0;
}
