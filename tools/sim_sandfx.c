// Host tests for SAND (open hourglass), with gravel alongside for reference
// (no ESP-IDF needed).
//
//   mkdir -p build-host && cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_sandfx.c -lm
//       -o build-host/sim_sandfx && build-host/sim_sandfx
//
// 1. Stream arc: a stream poured from the top, gravity turned 45 deg in one
//    step mid-fall; the mean velocity direction of the grains that were in
//    the air at the turn, over time.
// 2. Stream coherence: how wide the falling stream is and how much its
//    grains scatter sideways.
// 3. Pile angle: heap poured from a narrow column, upright and leaned back.
// 4. Rest: after a pour, a shake and a flip, time until every grain is
//    asleep with no input; and the total mechanical energy (kinetic +
//    potential in the gravity field) must never grow on its own.
// name=value arguments override sand parameters (tools/overrides.h).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "material.h"
#include "sim.h"
#include "overrides.h"

#define DEG (3.14159265f / 180.0f)

static sim_t sim;
static void *mem;
static material_t mats[MATERIAL_COUNT];
static int capacity; // grains the current sim was set up for

static void setup(int material, int n, uint32_t seed)
{
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, seed);
    sim_set_material(&sim, &mats[material]);
    capacity = n;
}

// Board leaned back `lean` deg and turned `rot` deg in its plane.
static void board(float lean, float rot)
{
    const float in_plane = cosf(lean * DEG);
    sim_set_gravity(&sim, in_plane * sinf(rot * DEG), in_plane * cosf(rot * DEG), -sinf(lean * DEG));
}

// Add a grain at (x, y) at rest if the cell is free.
static void add_grain(int x, int y, float vx, float vy)
{
    if (sim.cell[y * GRID_W + x] || sim.n >= capacity) {
        return;
    }
    sim_grain_t *g = &sim.grains[sim.n];
    g->x = x + 0.5f;
    g->y = y + 0.5f;
    g->vx = vx;
    g->vy = vy;
    sim.idle[sim.n] = 0;
    sim.ground[sim.n] = 0;
    sim.cell[y * GRID_W + x] = (uint16_t)(sim.n + 1);
    sim.n++;
}

// Total mechanical energy: sum of v^2/2 plus the potential -a.r of every grain
// (cells^2/step^2).
static double mech_energy(void)
{
    double e = 0;
    for (int k = 0; k < sim.n; k++) {
        const sim_grain_t *g = &sim.grains[k];
        e += 0.5 * ((double)g->vx * g->vx + (double)g->vy * g->vy) - ((double)sim.ax * g->x + (double)sim.ay * g->y);
    }
    return e;
}

static int asleep_all(void)
{
    return sim.awake == 0 && sim.motion == 0.0f;
}

// ---- 1. Stream arc -------------------------------------------------------------

static void test_arc(int material)
{
    const int cap = (int)(FILL_FRACTION * GRID_W * GRID_H);
    setup(material, cap, 1);
    sim.n = 0;
    board(0, 0);
    const int src = GRID_W / 2 - 20;
    // Pour for 1 s so the stream reaches the floor, then turn by 45 deg.
    for (int i = 0; i < SIM_HZ; i++) {
        add_grain(src, 1, 0, 0);
        add_grain(src + 1, 1, 0, 0);
        sim_step(&sim);
    }
    static uint8_t tracked[8192];
    int nt = 0;
    for (int k = 0; k < sim.n; k++) {
        const sim_grain_t *g = &sim.grains[k];
        tracked[k] = g->vy > 1.0f && g->y < GRID_H - 30;
        nt += tracked[k];
    }
    board(0, 45);
    printf("%s stream arc: %d grains in the air when gravity turns 45 deg (toward +x); their mean velocity:\n",
           mats[material].name, nt);
    printf("   t(ms)  direction (deg from straight down, + toward the new gravity)  speed  in the air\n");
    for (int i = 0; i <= SIM_HZ / 2; i++) {
        if (i % 6 == 0 || i == 1 || i == 2 || i == 3) {
            double vx = 0, vy = 0;
            int c = 0;
            for (int k = 0; k < sim.n; k++) {
                const sim_grain_t *g = &sim.grains[k];
                const int x = (int)g->x, y = (int)g->y;
                // Still flying: tracked and nothing directly ahead along the old or new down.
                if (tracked[k] && y < GRID_H - 1 && !sim.cell[(y + 1) * GRID_W + x] && (g->vx != 0 || g->vy != 0)) {
                    vx += g->vx;
                    vy += g->vy;
                    c++;
                }
            }
            if (c == 0) {
                printf("  %5.0f  (all landed)\n", (double)i * 1000.0 / SIM_HZ);
                break;
            }
            vx /= c;
            vy /= c;
            printf("  %5.0f  %+6.1f %50s %5.2f  %4d\n", (double)i * 1000.0 / SIM_HZ, atan2(vx, vy) * 180.0 / 3.14159265,
                   "", sqrt(vx * vx + vy * vy), c);
        }
        sim_step(&sim);
    }
}

// ---- 2. Stream coherence -----------------------------------------------------------

static void test_stream(int material)
{
    const int cap = (int)(FILL_FRACTION * GRID_W * GRID_H);
    setup(material, cap, 2);
    sim.n = 0;
    board(0, 0);
    const int src = GRID_W / 2;
    double width = 0, vxabs = 0, spread_max = 0;
    long samples = 0, nvx = 0;
    for (int i = 0; i < SIM_HZ * 2; i++) {
        add_grain(src, 1, 0, 0);
        add_grain(src + 1, 1, 0, 0);
        sim_step(&sim);
        if (i >= SIM_HZ / 2) {
            // Falling grains in rows 30..90 (before the heap): columns used, sideways speed.
            int xmin = GRID_W, xmax = -1;
            for (int k = 0; k < sim.n; k++) {
                const sim_grain_t *g = &sim.grains[k];
                if (g->y >= 30 && g->y < 90 && g->vy > 0.5f) {
                    const int x = (int)g->x;
                    xmin = x < xmin ? x : xmin;
                    xmax = x > xmax ? x : xmax;
                    vxabs += fabsf(g->vx);
                    nvx++;
                }
            }
            if (xmax >= 0) {
                width += xmax - xmin + 1;
                spread_max = xmax - xmin + 1 > spread_max ? xmax - xmin + 1 : spread_max;
                samples++;
            }
        }
    }
    printf("%s stream (2 wide at the source, rows 30-89 while pouring 2 s): width mean %.1f / max %.0f columns; "
           "sideways speed mean %.3f cells/step\n",
           mats[material].name, samples ? width / samples : 0.0, spread_max, nvx ? vxabs / nvx : 0.0);
}

// ---- 3. Pile angle -----------------------------------------------------------------

static void column_heights(int *h)
{
    for (int x = 0; x < GRID_W; x++) {
        h[x] = 0;
        for (int y = 0; y < GRID_H; y++) {
            if (sim.cell[y * GRID_W + x]) {
                h[x] = GRID_H - y;
                break;
            }
        }
    }
}

// Least-squares slope of the heap flanks between 20% and 80% of peak height
// (as tools/sim_host.c).
static float heap_angle(void)
{
    int h[GRID_W];
    column_heights(h);
    int peak = 0, px = 0;
    for (int x = 0; x < GRID_W; x++) {
        if (h[x] > peak) {
            peak = h[x];
            px = x;
        }
    }
    double sum = 0;
    int flanks = 0;
    for (int side = -1; side <= 1; side += 2) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int cnt = 0;
        for (int x = px; x >= 0 && x < GRID_W; x += side) {
            if (h[x] < peak * 0.2 || h[x] > peak * 0.8) {
                continue;
            }
            sx += x;
            sy += h[x];
            sxx += (double)x * x;
            sxy += (double)x * h[x];
            cnt++;
        }
        if (cnt >= 3) {
            double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
            sum += atan(fabs(slope)) * 180.0 / 3.14159265;
            flanks++;
        }
    }
    return flanks ? (float)(sum / flanks) : -1.0f;
}

static void pour_heap(int material, int n, float lean, uint32_t seed)
{
    setup(material, n, seed);
    sim.n = 0;
    board(lean, 0);
    while (sim.n < n) {
        for (int k = -1; k <= 1; k++) {
            add_grain(GRID_W / 2 + k, 1, 0, 0);
        }
        sim_step(&sim);
    }
    for (int i = 0; i < 3000; i++) {
        sim_step(&sim);
    }
}

static void test_heap(int material)
{
    printf("%s heap (1500 grains poured from a 3-wide column) angle:", mats[material].name);
    const float leans[] = { 0, 30, 45, 60, 75 };
    for (int l = 0; l < 5; l++) {
        double a = 0;
        for (int r = 0; r < 3; r++) {
            pour_heap(material, 1500, leans[l], 100 + (uint32_t)r);
            a += heap_angle();
        }
        printf("  lean %2.0f: %.1f deg", (double)leans[l], a / 3);
    }
    printf("  (3 seeds each)\n");
}

// ---- 4. Rest -----------------------------------------------------------------------

// Run with no input until everything sleeps (max `seconds`). Returns the rest
// time (s) or -1, and reports the largest single-step and 0.5 s window rise of
// the mechanical energy.
static float last_rise; // time (s) of the last step on which the energy rose

static float settle(float seconds, double *rise_step, double *rise_window, int *rises)
{
    last_rise = -1.0f;
    const int steps = (int)(seconds * SIM_HZ);
    double prev = mech_energy(), win_prev = prev;
    *rise_step = *rise_window = 0;
    *rises = 0;
    for (int i = 1; i <= steps; i++) {
        sim_step(&sim);
        const double e = mech_energy();
        if (e - prev > *rise_step) {
            *rise_step = e - prev;
        }
        if (e > prev + 1e-3) {
            (*rises)++;
            last_rise = (float)i / SIM_HZ;
        }
        prev = e;
        if (i % (SIM_HZ / 2) == 0) {
            if (e - win_prev > *rise_window) {
                *rise_window = e - win_prev;
            }
            win_prev = e;
        }
        if (asleep_all()) {
            return (float)i / SIM_HZ;
        }
    }
    return -1.0f;
}

static void report_rest(const char *what, int material, float t, double rs, double rw, int rises, double e0)
{
    printf("%s %-26s at full rest (all asleep) after %s", mats[material].name, what, t < 0 ? "NEVER (20 s)" : "");
    if (t >= 0) {
        printf("%.2f s", (double)t);
    }
    printf("; mechanical energy (start %.0f) rose on %d steps (last at %.2f s), largest step rise %.3f, largest "
           "0.5 s rise %.3f\n",
           e0, rises, (double)last_rise, rs, rw);
}

static void test_rest(int material)
{
    const int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    double rs, rw, e0;
    int rises;
    float t;

    // Pour: 2 s of a 3-wide stream onto the floor, then hands off.
    setup(material, n, 7);
    sim.n = 0;
    board(0, 0);
    for (int i = 0; i < SIM_HZ * 2; i++) {
        for (int k = -1; k <= 1; k++) {
            add_grain(GRID_W / 2 + k, 1, 0, 0);
        }
        sim_step(&sim);
    }
    e0 = mech_energy();
    t = settle(20, &rs, &rw, &rises);
    report_rest("after a 2 s pour:", material, t, rs, rw, rises, e0);

    // Shake: a settled bed shaken every other step for 0.25 s.
    setup(material, n, 8);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    for (int i = 0; i < 3000; i++) {
        sim_step(&sim);
    }
    for (int i = 0; i < SIM_HZ / 4; i++) {
        if ((i & 1) == 0) {
            sim_shake(&sim, (i & 2) ? 1.0f : -1.0f, -1.0f, SHAKE_STRENGTH);
        }
        sim_step(&sim);
    }
    e0 = mech_energy();
    t = settle(20, &rs, &rw, &rises);
    report_rest("after a 0.25 s shake:", material, t, rs, rw, rises, e0);

    // Flip: a settled bed turned upside down in one step.
    setup(material, n, 9);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    for (int i = 0; i < 3000; i++) {
        sim_step(&sim);
    }
    board(0, 180);
    e0 = mech_energy();
    t = settle(20, &rs, &rw, &rises);
    report_rest("after a flip:", material, t, rs, rw, rises, e0);
}

int main(int argc, char **argv)
{
    for (int m = 0; m < MATERIAL_COUNT; m++) {
        mats[m] = *material_get(m);
    }
    for (int a = 1; a < argc; a++) {
        override_apply(&mats[MATERIAL_SAND], argv[a]);
    }
    override_print(material_get(MATERIAL_SAND), &mats[MATERIAL_SAND]);
    const int order[] = { MATERIAL_CLASSIC, MATERIAL_SAND };
    for (int k = 0; k < 2; k++) {
        test_arc(order[k]);
    }
    for (int k = 0; k < 2; k++) {
        test_stream(order[k]);
    }
    for (int k = 0; k < 2; k++) {
        test_heap(order[k]);
    }
    for (int k = 0; k < 2; k++) {
        test_rest(order[k]);
    }
    free(mem);
    return 0;
}
