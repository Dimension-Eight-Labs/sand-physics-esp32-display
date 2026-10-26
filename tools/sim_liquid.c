// Host tests for material LIQUID, material switching and sleep/wake under
// hand tremor (no ESP-IDF needed).
//
//   cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_liquid.c -lm -o sim_liquid && ./sim_liquid
//
// Gravity is given in the screen frame like the IMU delivers it (x right,
// y down, z out of the glass). Times are at SIM_HZ.
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
static int n_grains;

static void setup(uint32_t seed)
{
    n_grains = (int)(FILL_FRACTION * GRID_W * GRID_H);
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n_grains));
    sim_init(&sim, mem, GRID_W, GRID_H, n_grains, seed);
}

static void board(float lean_deg, float rot_deg)
{
    float in_plane = cosf(lean_deg * DEG);
    sim_set_gravity(&sim, in_plane * sinf(rot_deg * DEG), in_plane * cosf(rot_deg * DEG), -sinf(lean_deg * DEG));
}

static void run(int steps)
{
    for (int i = 0; i < steps; i++) {
        sim_step(&sim);
    }
}

// Surface relative to gravity rotated by rot_deg in the screen plane: the top
// of every screen column holding liquid, compared with the ideal level line
// through their mean. Returns the spread (max - min) of that deviation in
// cells perpendicular to the level surface, and the fitted surface angle in
// *ret_angle.
static float flatness(float rot_deg, float *ret_angle)
{
    // Height = rows from the bottom to the top grain of each column. With
    // gravity rotated by rot (toward +x for rot > 0) the level surface rises
    // toward the lowered edge with slope tan(rot).
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    static int h[GRID_W];
    for (int x = 0; x < GRID_W; x++) {
        h[x] = -1;
        for (int y = 0; y < GRID_H; y++) {
            if (sim.cell[y * GRID_W + x]) {
                h[x] = GRID_H - y;
                break;
            }
        }
        if (h[x] > 0 && h[x] < GRID_H) {
            sx += x;
            sy += h[x];
            sxx += (double)x * x;
            sxy += (double)x * h[x];
            cnt++;
        }
    }
    double slope = cnt > 2 ? (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx) : 0;
    if (ret_angle) {
        *ret_angle = (float)(atan(slope) * 180.0 / 3.14159265);
    }
    // Residual against the ideal level line through the mean.
    double ideal = tan((double)rot_deg * 3.14159265 / 180.0);
    double mx = cnt ? sx / cnt : 0, my = cnt ? sy / cnt : 0;
    double lo = 1e9, hi = -1e9;
    for (int x = 0; x < GRID_W; x++) {
        if (h[x] > 0 && h[x] < GRID_H) {
            double r = h[x] - (my + ideal * (x - mx));
            lo = r < lo ? r : lo;
            hi = r > hi ? r : hi;
        }
    }
    // Measured vertically; perpendicular to the level surface that is cos(rot)
    // of it (so a 45 degree surface isn't penalised for its staircase).
    return cnt ? (float)((hi - lo) * cos((double)rot_deg * 3.14159265 / 180.0)) : 0.0f;
}

// Steps until nothing changes cell for 60 steps and motion is ~0.
static int settle(int max_steps)
{
    static uint16_t prev[GRID_W * GRID_H];
    int quiet = 0;
    for (int i = 1; i <= max_steps; i++) {
        memcpy(prev, sim.cell, sizeof(prev));
        sim_step(&sim);
        quiet = memcmp(prev, sim.cell, sizeof(prev)) == 0 && sim.motion < 1.0f ? quiet + 1 : 0;
        if (quiet >= 60) {
            return i - 60;
        }
    }
    return -1;
}

static int cells_changed(const uint16_t *before)
{
    int moved = 0;
    for (int i = 0; i < GRID_W * GRID_H; i++) {
        moved += (before[i] != 0) != (sim.cell[i] != 0);
    }
    return moved / 2;
}

// Pour a heap from a narrow column (classic: a ~33° pile).
static void pour_heap(int n)
{
    sim.n = 0;
    board(0, 0);
    int cx = GRID_W / 2;
    while (sim.n < n) {
        for (int k = -1; k <= 1 && sim.n < n; k++) {
            int x = cx + k, y = 1;
            if (sim.cell[y * GRID_W + x] == 0) {
                sim_grain_t *g = &sim.grains[sim.n];
                g->x = x + 0.5f;
                g->y = y + 0.5f;
                g->vx = g->vy = 0;
                sim.cell[y * GRID_W + x] = (uint16_t)(sim.n + 1);
                sim.n++;
            }
        }
        sim_step(&sim);
    }
    run(2000);
}

// Simulated IMU: 200 Hz samples of `base` plus uniform +/-noise on each axis
// and a slow sway, through the firmware's low-pass; one sim step per 1/120 s.
typedef struct {
    float lp[3];
    double t;
    unsigned seed;
} imu_sim_t;

static float urand(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (float)((*seed >> 8) & 0xFFFF) / 32767.5f - 1.0f;
}

static void imu_feed(imu_sim_t *im, const float base[3], float noise, float sway_deg, float dt)
{
    const double imu_dt = 1.0 / IMU_POLL_HZ;
    double end = im->t + (double)dt;
    while (im->t < end) {
        // Sway: slow rotation of the board in its plane (hand drift).
        float a = sway_deg * DEG * sinf((float)(im->t * 2.0 * 3.14159265 * 0.7));
        float g[3] = {
            base[0] * cosf(a) - base[1] * sinf(a) + noise * urand(&im->seed),
            base[0] * sinf(a) + base[1] * cosf(a) + noise * urand(&im->seed),
            base[2] + noise * urand(&im->seed),
        };
        for (int i = 0; i < 3; i++) {
            im->lp[i] += IMU_LPF_ALPHA * (g[i] - im->lp[i]);
        }
        im->t += imu_dt;
    }
}

static material_t wm; // liquid, possibly with command-line overrides

static const material_t *mat_of(int material)
{
    return material == MATERIAL_LIQUID ? &wm : material_get(material);
}

static void tremor_test(int material, float noise, float sway_deg)
{
    setup(99);
    sim_set_material(&sim, mat_of(material));
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    settle(20000);
    imu_sim_t im = { .lp = { 0, 1, 0 }, .seed = 5 };
    const float base[3] = { 0, 1, 0 };
    long awake = 0, steps = 0;
    for (int i = 0; i < 6 * SIM_HZ; i++) {
        imu_feed(&im, base, noise, sway_deg, 1.0f / SIM_HZ);
        sim_set_gravity(&sim, im.lp[0], im.lp[1], im.lp[2]);
        sim_step(&sim);
        if (i >= 2 * SIM_HZ) { // skip the first 2 s
            awake += sim.awake;
            steps++;
        }
    }
    printf("  %-7s tremor +/-%.2f g, sway +/-%.1f deg: %5ld of %d grains awake on average\n",
           material_get(material)->name, (double)noise, (double)sway_deg, awake / steps, sim.n);
}

// A deliberate tilt during tremor: steps until the material reacts. Sand
// wakes its whole pile; liquid wakes only grains that can move, so for it the
// reaction is the first step in which grains change cell.
static void tilt_response(int material, float tilt_deg)
{
    setup(99);
    sim_set_material(&sim, mat_of(material));
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    settle(20000);
    imu_sim_t im = { .lp = { 0, 1, 0 }, .seed = 9 };
    float base[3] = { 0, 1, 0 };
    for (int i = 0; i < 2 * SIM_HZ; i++) {
        imu_feed(&im, base, 0.03f, 0.0f, 1.0f / SIM_HZ);
        sim_set_gravity(&sim, im.lp[0], im.lp[1], im.lp[2]);
        sim_step(&sim);
    }
    int before = sim.awake;
    int woke_at = -1, moved_at = -1, peak_awake = 0;
    uint32_t m0 = sim.moves;
    for (int i = 0; i < SIM_HZ; i++) {
        float a = tilt_deg * DEG * fminf(1.0f, (float)i / (0.1f * SIM_HZ));
        base[0] = sinf(a);
        base[1] = cosf(a);
        imu_feed(&im, base, 0.03f, 0.0f, 1.0f / SIM_HZ);
        uint32_t mv = sim.moves;
        sim_set_gravity(&sim, im.lp[0], im.lp[1], im.lp[2]);
        sim_step(&sim);
        peak_awake = sim.awake > peak_awake ? sim.awake : peak_awake;
        if (woke_at < 0 && sim.awake > sim.n / 2) {
            woke_at = i;
        }
        if (moved_at < 0 && sim.moves != mv) {
            moved_at = i;
        }
    }
    printf("  %-7s %+.0f deg tilt over 0.1 s: %d awake before; first grains move after %d steps (%.0f ms); "
           "peak %d awake; %u cell moves in 1 s\n",
           mat_of(material)->name, (double)tilt_deg, before, moved_at, moved_at * 1000.0 / SIM_HZ, peak_awake,
           sim.moves - m0);
    (void)woke_at;
}

static float time_to_level(float rot_deg, float tol, float limit)
{
    for (int i = 1; i <= (int)(limit * 20.0f); i++) {
        run(SIM_HZ / 20);
        if (flatness(rot_deg, NULL) <= tol) {
            return (float)i * 0.05f;
        }
    }
    return -1.0f;
}

// One-line summary for a liquid parameter set (used to tune it).
static void sweep_line(void)
{
    setup(11);
    sim_set_material(&sim, &wm);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    settle(20000);
    board(0, 45);
    float t45 = time_to_level(45, 2.0f, 20.0f);
    settle(20000);
    float a45;
    float f45 = flatness(45, &a45);
    printf("   (45 deg settled: surface %.1f deg, deviation %.1f cells)\n", (double)a45, (double)f45);
    board(0, 0);
    float t0 = time_to_level(0, 2.0f, 20.0f);
    int rest = settle(20000);
    float f0 = flatness(0, NULL);
    setup(21);
    pour_heap(1800);
    sim_set_material(&sim, &wm);
    float th = time_to_level(0, 2.0f, 20.0f);
    printf("lvl %d/%d damp %.3f step %d push %.2f scan %2d maxv %.1f cf %.2f e %.2f | 45 deg level(<=2): %5.2f s | back level(<=2): %5.2f s, "
           "rest +%.1f s, final %.0f | heap->liquid level: %5.2f s\n",
           wm.level_rate, wm.level_range, (double)wm.damping, wm.spread_step, (double)wm.spread_push, wm.spread_scan, (double)wm.max_speed, (double)wm.contact_friction,
           (double)wm.elasticity, (double)t45, (double)t0, (double)rest / SIM_HZ, (double)f0, (double)th);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "sweep") == 0) {
        // sim_liquid sweep push scan max_speed contact_friction elasticity
        wm = *material_get(MATERIAL_LIQUID);
        wm.spread_push = (float)atof(argv[2]);
        wm.spread_scan = atoi(argv[3]);
        wm.max_speed = (float)atof(argv[4]);
        wm.contact_friction = (float)atof(argv[5]);
        wm.elasticity = (float)atof(argv[6]);
        wm.spread_step = argc > 7 ? atoi(argv[7]) : wm.spread_step;
        wm.damping = argc > 8 ? (float)atof(argv[8]) : wm.damping;
        wm.level_rate = argc > 9 ? atoi(argv[9]) : wm.level_rate;
        wm.level_range = argc > 10 ? atoi(argv[10]) : wm.level_range;
        sweep_line();
        return 0;
    }
    // name=value arguments override liquid parameters (tools/overrides.h).
    wm = *material_get(MATERIAL_LIQUID);
    for (int a = 1; a < argc; a++) {
        if (!override_apply(&wm, argv[a])) {
            fprintf(stderr, "usage: sim_liquid [name=value ...] | sim_liquid sweep push scan max_speed cf e ...\n");
            return 2;
        }
    }
    override_print(material_get(MATERIAL_LIQUID), &wm);
    const material_t *w = &wm;
    printf("material %s: elasticity %.2f, damping %.3f, contact friction %.2f, glass friction %.2f, "
           "spread scan %d, push %.2f, max speed %.1f\n",
           w->name, (double)w->elasticity, (double)w->damping, (double)w->contact_friction,
           (double)w->glass_friction, w->spread_scan, (double)w->spread_push, (double)w->max_speed);

    // 1. Liquid pool: settle, tilt 45 deg, then back to upright.
    setup(11);
    sim_set_material(&sim, w);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    int t0 = settle(20000);
    float ang;
    printf("liquid spawn settles in %.2f s, flatness %.1f cells\n", (double)t0 / SIM_HZ, (double)flatness(0, &ang));

    // Track flatness every 0.05 s; print every 0.25 s for 2 s, then each second.
    for (int phase = 0; phase < 2; phase++) {
        float rot = phase == 0 ? 45.0f : 0.0f;
        board(0, rot);
        printf(phase == 0 ? "tilt to 45 deg:\n" : "back to upright:\n");
        float level_at = -1.0f;
        for (int i = 1; i <= 100; i++) {
            run(SIM_HZ / 20);
            float f = flatness(rot, &ang);
            if (level_at < 0 && f <= 2.0f) {
                level_at = (float)i * 0.05f;
            }
            if ((i <= 40 && i % 5 == 0) || i % 20 == 0) {
                printf("  t=%.2f s  surface %5.1f deg (level %4.1f)  deviation %4.1f cells  awake %4d  motion %.0f\n",
                       i * 0.05, (double)ang, (double)rot, (double)f, sim.awake, (double)sim.motion);
            }
        }
        float t_in = 5.0f;
        int t_rest = settle(20000);
        float f_end = flatness(rot, &ang);
        if (level_at > 0) {
            printf("  level within 2 cells after %.2f s; at rest after %.2f s; final deviation %.1f cells, "
                   "surface %.1f deg, %d awake\n",
                   (double)level_at, (double)(t_in + (float)t_rest / SIM_HZ), (double)f_end, (double)ang, sim.awake);
        } else {
            printf("  NOT level within 2 cells in 5 s; at rest after %.2f s; final deviation %.1f cells\n",
                   (double)(t_in + (float)t_rest / SIM_HZ), (double)f_end);
        }
    }
    float level_at;

    // 2. Classic heap -> liquid: slumps flat.
    setup(21);
    pour_heap(1800);
    float before_f = flatness(0, NULL);
    sim_set_material(&sim, w);
    printf("classic heap (height difference %.0f cells) switched to liquid:\n", (double)before_f);
    level_at = -1.0f;
    for (int i = 1; i <= 80; i++) {
        run(SIM_HZ / 20);
        float f = flatness(0, NULL);
        if (level_at < 0 && f <= 2.0f) {
            level_at = (float)i * 0.05f;
        }
        if ((i <= 20 && i % 2 == 0) || i % 20 == 0) {
            printf("  t=%.2f s  max height difference %4.1f cells\n", i * 0.05, (double)f);
        }
    }
    if (level_at > 0) {
        printf("  level within 2 cells after %.2f s\n", (double)level_at);
    } else {
        printf("  NOT level within 2 cells in 4 s\n");
    }

    // 3. Liquid pool -> classic: rests, then piles on the next tilt.
    setup(31);
    sim_set_material(&sim, w);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    settle(20000);
    static uint16_t snap[GRID_W * GRID_H];
    memcpy(snap, sim.cell, sizeof(snap));
    sim_set_material(&sim, material_get(MATERIAL_CLASSIC));
    run(SIM_HZ * 2);
    printf("liquid pool switched to classic: %d grains moved in 2 s, %d awake after\n", cells_changed(snap),
           sim.awake);
    board(0, 50);
    run(8000);
    float a = 0;
    flatness(50, &a);
    printf("  then rotated 50 deg: surface %.1f deg -> effective repose %.1f deg (classic from sand: 31.0)\n",
           (double)a, (double)(50.0f - a));

    // 4. Liquid lying flat with sensor noise.
    setup(41);
    sim_set_material(&sim, w);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    board(0, 0);
    settle(20000);
    memcpy(snap, sim.cell, sizeof(snap));
    srand(1);
    long aw = 0;
    for (int i = 0; i < 2000; i++) {
        float nx = ((rand() % 2001) - 1000) / 1000.0f * 0.03f;
        float ny = ((rand() % 2001) - 1000) / 1000.0f * 0.03f;
        sim_set_gravity(&sim, nx, ny, -1.0f);
        sim_step(&sim);
        aw += sim.awake;
    }
    printf("liquid lying flat + 0.03 g noise, 2000 steps: %d grains moved, %ld awake on average\n",
           cells_changed(snap), aw / 2000);
    // Freeze point for liquid (lower glass friction than sand).
    for (int lean = 80; lean <= 88; lean += 2) {
        setup(42);
        sim_set_material(&sim, w);
        sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
        board(0, 0);
        settle(20000);
        memcpy(snap, sim.cell, sizeof(snap));
        board((float)lean, 90);
        run(1200);
        printf("  liquid lean %d deg, turned 90: %d grains moved\n", lean, cells_changed(snap));
    }

    // 5. Sleep/wake with a hand-held board.
    printf("settled pile held upright with hand tremor (6 s, last 4 s averaged):\n");
    for (int m = 0; m < MATERIAL_COUNT; m++) {
        tremor_test(m, 0.03f, 0.0f);
        tremor_test(m, 0.03f, 1.0f);
    }
    printf("deliberate tilt during tremor:\n");
    for (int m = 0; m < MATERIAL_COUNT; m++) {
        tilt_response(m, 5.0f);
    }
    free(mem);
    return 0;
}
