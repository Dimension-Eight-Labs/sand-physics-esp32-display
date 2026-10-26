// Host test: does disturbed liquid always come back to rest on its own?
// (no ESP-IDF needed)
//
//   mkdir -p build-host && cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_liquidstab.c -lm
//       -o build-host/sim_liquidstab && build-host/sim_liquidstab
//
// 1. Repro: a settled pool knocked several times in a row (spray landing back
//    into a disturbed surface), then 40 s with no input. Every 0.5 s: variance
//    of the column heights, wave flow energy (sum of squared column-to-column
//    flows), wave state, levelling transfers, spread pushes, awake grains,
//    motion. Upright and held at 30 deg (a diagonal gravity frame) or
//    -120 deg, upright or leaned back 20 deg.
// 2. Random: 60 s of random knocks, tilts and pours, then 10 s with no
//    input; repeated with several seeds. Must reach rest every time.
// name=value arguments override liquid parameters (tools/overrides.h);
// "quick" runs fewer random seeds; "runN" traces random run N only (and
// "runN long" gives it 40 s of quiet).
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
static material_t wm;
static float lean_deg; // board leaned back (glass friction), 0 = upright
static int cap;        // grain capacity (pours add grains)

static void gravity(float rot_deg)
{
    const float in_plane = cosf(lean_deg * DEG);
    sim_set_gravity(&sim, in_plane * sinf(rot_deg * DEG), in_plane * cosf(rot_deg * DEG), sinf(lean_deg * DEG));
}

// Settled pool of n grains (capacity cap for pours).
static void pool(int n, uint32_t seed)
{
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, cap));
    sim_init(&sim, mem, GRID_W, GRID_H, cap, seed);
    sim_set_material(&sim, &wm);
    sim.n = 0;
    for (int y = GRID_H - 1; y >= 0 && sim.n < n; y--) {
        for (int x = 0; x < GRID_W && sim.n < n; x++) {
            sim_grain_t *g = &sim.grains[sim.n];
            g->x = x + 0.5f;
            g->y = y + 0.5f;
            g->vx = g->vy = 0.0f;
            sim.cell[y * GRID_W + x] = (uint16_t)(sim.n + 1);
            sim.n++;
        }
    }
    gravity(0);
    for (int i = 0; i < 1500; i++) {
        sim_step(&sim);
    }
}

// Height of the body in column x (run of grains packed against the floor,
// upright frame).
static int column_height(int x)
{
    int y = GRID_H - 1;
    while (y >= 0 && sim.cell[y * GRID_W + x]) {
        y--;
    }
    return GRID_H - 1 - y;
}

static double height_variance(void)
{
    double s = 0, s2 = 0;
    for (int x = 0; x < GRID_W; x++) {
        double h = column_height(x);
        s += h;
        s2 += h * h;
    }
    const double mean = s / GRID_W;
    return s2 / GRID_W - mean * mean;
}

static double wave_energy(void)
{
    double e = 0;
    for (int k = 0; k < GRID_W + GRID_H; k++) {
        e += (double)sim.flux[k] * sim.flux[k];
    }
    return e;
}

// One knock as the IMU delivers it: 4 impulses on consecutive steps.
static void knock(float dir_x, float scale)
{
    static const float profile[] = { 1.0f, 0.8f, 0.5f, 0.3f };
    for (int i = 0; i < 4; i++) {
        sim_shake(&sim, dir_x, 0.0f, SHAKE_STRENGTH * scale * profile[i]);
        sim_step(&sim);
    }
}

// At rest: nothing awake, no motion, no levelling move in the last step.
static int at_rest(uint32_t transfers_before)
{
    return sim.awake == 0 && sim.motion == 0.0f && sim.n_transfers == transfers_before;
}

// Run `seconds` with no input. Returns the time of full rest (s, -1 = never);
// prints a line every `every` steps when every > 0.
static float quiet(float seconds, int every, int *backstop)
{
    const int steps = (int)(seconds * SIM_HZ);
    uint32_t t0 = sim.n_transfers, p0 = sim.n_pushes, b0 = sim.n_wave_backstop;
    float rest_at = -1.0f;
    double emax = 0;
    for (int i = 1; i <= steps; i++) {
        const uint32_t tb = sim.n_transfers;
        sim_step(&sim);
        if (at_rest(tb)) {
            if (rest_at < 0) {
                rest_at = (float)i / SIM_HZ;
            }
        } else {
            rest_at = -1.0f;
        }
        double e = wave_energy();
        emax = e > emax ? e : emax;
        if (every > 0 && i % every == 0) {
            printf("    t=%5.1f s  height var %7.2f  wave energy %8.3f (max %8.3f)  waves %s  transfers %5u  "
                   "pushes %4u  awake %4d  motion %7.1f\n",
                   (double)i / SIM_HZ, height_variance(), e, emax, sim.wave_active ? "on " : "off",
                   sim.n_transfers - t0, sim.n_pushes - p0, sim.awake, (double)sim.motion);
            t0 = sim.n_transfers;
            p0 = sim.n_pushes;
            emax = 0;
        }
    }
    if (backstop) {
        *backstop = (int)(sim.n_wave_backstop - b0);
    }
    return rest_at;
}

static void repro(float lean, float rot, int knocks, uint32_t seed)
{
    lean_deg = lean;
    pool((int)(0.30f * GRID_W * GRID_H), seed);
    if (rot != 0.0f) {
        // Turn to the held attitude over 0.5 s and let it settle there.
        for (int i = 1; i <= SIM_HZ / 2; i++) {
            gravity(rot * (float)i / (SIM_HZ / 2));
            sim_step(&sim);
        }
        quiet(10.0f, 0, NULL);
    }
    printf("repro: settled pool, held at %.0f deg, leaned back %.0f deg; %d knocks 0.25 s apart, then 40 s with "
           "no input\n",
           (double)rot, (double)lean, knocks);
    for (int k = 0; k < knocks; k++) {
        knock(k & 1 ? -1.0f : 1.0f, 1.0f);
        for (int i = 0; i < SIM_HZ / 4 - 4; i++) {
            sim_step(&sim);
        }
    }
    int bs = 0;
    float r = quiet(40.0f, SIM_HZ * 2, &bs);
    if (r < 0) {
        printf("  full rest: NEVER (still moving at 40 s)");
    } else {
        printf("  full rest after %.2f s", (double)r);
    }
    printf("; backstop triggered %d times\n", bs);
}

static uint32_t rng_state;
static float frand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(rng_state >> 8) / 16777216.0f;
}

// 60 s of random input, then 10 s quiet. Returns the rest time in the quiet
// part (-1 = never).
static float random_run(uint32_t seed, int *backstop, int *knocks, int *pours, int *tilts, int every, float quiet_s)
{
    rng_state = seed * 2654435761u + 1;
    lean_deg = frand() < 0.5f ? 0.0f : 20.0f;
    pool((int)(0.22f * GRID_W * GRID_H), seed);
    float rot = 0.0f, rot_target = 0.0f;
    int pour_left = 0, src = GRID_W / 2;
    *knocks = *pours = *tilts = 0;
    for (int i = 0; i < SIM_HZ * 60; i++) {
        // New events at random.
        if (frand() < 1.0f / SIM_HZ) { // ~1 knock per second
            const float scale = 0.4f + 0.8f * frand();
            knock(frand() < 0.5f ? -1.0f : 1.0f, scale);
            (*knocks)++;
            i += 4;
        }
        if (frand() < 0.4f / SIM_HZ) { // a tilt now and then, sometimes big
            rot_target = (frand() - 0.5f) * (frand() < 0.2f ? 360.0f : 70.0f);
            (*tilts)++;
        }
        if (pour_left == 0 && frand() < 0.25f / SIM_HZ && sim.n < cap - 400) {
            pour_left = SIM_HZ * (1 + (int)(frand() * 3));
            src = 2 + (int)(frand() * (GRID_W - 5));
            (*pours)++;
        }
        // Turn toward the target at up to 2 deg per step.
        float d = rot_target - rot;
        rot += d > 2.0f ? 2.0f : d < -2.0f ? -2.0f : d;
        gravity(rot);
        if (pour_left > 0) {
            pour_left--;
            // Pour from the edge opposite gravity (screen top when upright).
            for (int x = src; x <= src + 1; x++) {
                const int y = 1;
                if (!sim.cell[y * GRID_W + x] && sim.n < cap) {
                    sim_grain_t *g = &sim.grains[sim.n];
                    g->x = x + 0.5f;
                    g->y = y + 0.5f;
                    g->vx = 0.0f;
                    g->vy = 0.0f;
                    sim.idle[sim.n] = 0;
                    sim.ground[sim.n] = 0;
                    sim.cell[y * GRID_W + x] = (uint16_t)(sim.n + 1);
                    sim.n++;
                }
            }
        }
        sim_step(&sim);
    }
    // Back to a resting attitude, then hands off.
    while (fabsf(rot - rot_target) > 0.0f) {
        float d = rot_target - rot;
        rot += d > 2.0f ? 2.0f : d < -2.0f ? -2.0f : d;
        gravity(rot);
        sim_step(&sim);
    }
    if (every) {
        printf("  hands off at rotation %.0f deg (down = %d), %d grains\n", (double)rot, sim.down, sim.n);
    }
    return quiet(quiet_s, every, backstop);
}

int main(int argc, char **argv)
{
    wm = *material_get(MATERIAL_LIQUID);
    int quick = 0, only = -1, longq = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "quick")) {
            quick = 1;
        } else if (!strncmp(argv[a], "run", 3)) {
            only = atoi(argv[a] + 3);
        } else if (!strcmp(argv[a], "long")) {
            longq = 1;
        } else {
            override_apply(&wm, argv[a]);
        }
    }
    override_print(material_get(MATERIAL_LIQUID), &wm);
    cap = (int)(FILL_FRACTION * GRID_W * GRID_H);
    if (only >= 0) {
        int bs, kn, po, ti;
        float t = random_run(100 + only, &bs, &kn, &po, &ti, SIM_HZ / 2, longq ? 40.0f : 10.0f);
        printf("run %d: %d knocks, %d tilts, %d pours; rest %.2f s; backstop %d\n", only, kn, ti, po, (double)t, bs);
        return 0;
    }

    repro(0.0f, 0.0f, 5, 1);
    repro(20.0f, 0.0f, 5, 2);
    repro(0.0f, 30.0f, 5, 3);
    repro(20.0f, 30.0f, 5, 4);
    repro(20.0f, -120.0f, 5, 5);

    const int runs = quick ? 4 : 20;
    int never = 0, bs_total = 0;
    float worst = 0.0f;
    printf("random: %d runs of 60 s random knocks / tilts / pours, then 10 s with no input\n", runs);
    for (int r = 0; r < runs; r++) {
        int bs, kn, po, ti;
        float t = random_run(100 + r, &bs, &kn, &po, &ti, 0, 10.0f);
        printf("  run %2d (lean %2.0f, %d grains, %3d knocks, %2d tilts, %d pours): %s", r, (double)lean_deg, sim.n,
               kn, ti, po, t < 0 ? "NOT AT REST after 10 s" : "rest after");
        if (t >= 0) {
            printf(" %.2f s", (double)t);
            worst = t > worst ? t : worst;
        } else {
            never++;
        }
        printf("; backstop %d\n", bs);
        bs_total += bs;
    }
    printf("random: %d of %d runs reached rest (slowest %.2f s); backstop triggered %d times in total\n",
           runs - never, runs, (double)worst, bs_total);
    free(mem);
    return 0;
}
