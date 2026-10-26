// PC test harness for the sand simulation (no ESP-IDF needed).
//
//   cc -std=c11 -O2 -Wall -Wextra -Isrc src/sim.c tools/sim_host.c -lm -o sim_host
//   ./sim_host            run all checks
//   ./sim_host ascii      also print the heap
//
// Checks, for the board held upright and leaned back: angle of repose of a
// heap poured from a narrow column, how long a fresh spawn takes to settle,
// avalanche behaviour when the board is rotated in its plane; plus stillness
// lying flat with sensor noise, tipping past flat, shake recovery and raw
// step throughput.
//
// Gravity is given in the screen frame like the IMU delivers it:
// x right, y down the screen, z out of the glass.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "sim.h"

static sim_t sim;
static void *mem;

static void setup(int n)
{
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 12345);
}

static void run(int steps)
{
    for (int i = 0; i < steps; i++) {
        sim_step(&sim);
    }
}

#define DEG (3.14159265f / 180.0f)

// Board leaned back from vertical by `lean` degrees and rotated in its own
// plane by `rot` degrees (positive = right edge lowered).
static void set_board(float lean_deg, float rot_deg)
{
    float in_plane = cosf(lean_deg * DEG);
    sim_set_gravity(&sim, in_plane * sinf(rot_deg * DEG), in_plane * cosf(rot_deg * DEG),
                    -sinf(lean_deg * DEG));
}

// Pour grains from a narrow column at the centre top, a few per step.
static void pour_heap(int n, float lean_deg)
{
    setup(n);
    sim.n = 0;
    set_board(lean_deg, 0);
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
    run(3000);
}

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

// Least-squares slope of the heap flanks between 20% and 80% of peak height.
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

// Angle of the whole bed surface (columns away from the walls), positive when
// it rises to the right.
static float surface_angle(void)
{
    int h[GRID_W];
    column_heights(h);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    for (int x = 8; x < GRID_W - 8; x++) {
        sx += x;
        sy += h[x];
        sxx += (double)x * x;
        sxy += (double)x * h[x];
        cnt++;
    }
    double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    return (float)(atan(slope) * 180.0 / 3.14159265);
}

static void print_ascii(void)
{
    for (int y = GRID_H - 50; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            putchar(sim.cell[y * GRID_W + x] ? '#' : '.');
        }
        putchar('\n');
    }
}

static int snapshot_moved(const uint16_t *before)
{
    int moved = 0;
    for (int i = 0; i < GRID_W * GRID_H; i++) {
        moved += (before[i] != 0) != (sim.cell[i] != 0);
    }
    return moved / 2;
}

static float mean_y(void)
{
    double sum = 0;
    for (int i = 0; i < sim.n; i++) {
        sum += (double)sim.grains[i].y;
    }
    return (float)(sum / sim.n);
}

// Spawn at the top and run until the sand stops; returns steps taken.
static int spawn_and_settle(float lean_deg, int max_steps)
{
    int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    setup(n);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    set_board(lean_deg, 0);
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

// Settle a full spawn with the board upright.
static void settled_bed(void)
{
    spawn_and_settle(0, 20000);
}

int main(int argc, char **argv)
{
    int ascii = argc > 1 && strcmp(argv[1], "ascii") == 0;
    static uint16_t before[GRID_W * GRID_H];

    printf("grid %dx%d, REPOSE_ANGLE_DEG %.1f, BACK_FRICTION %.2f, SIM_HZ %d\n", GRID_W, GRID_H,
           (double)REPOSE_ANGLE_DEG, (double)BACK_FRICTION, SIM_HZ);

    // 1. Heap angle and settling time, upright and leaned back.
    static const float leans[] = { 0, 30, 60, 75 };
    for (unsigned i = 0; i < sizeof(leans) / sizeof(leans[0]); i++) {
        float lean = leans[i];
        pour_heap(1800, lean);
        float angle = heap_angle();
        if (ascii && (lean == 0 || lean == 60)) {
            print_ascii();
        }
        int steps = spawn_and_settle(lean, 20000);
        printf("lean %2.0f deg (pull %.2f g, glass %.2f g): heap %.1f deg, spawn settles in %d steps (%.1f s)\n",
               (double)lean, (double)cosf(lean * DEG), (double)(BACK_FRICTION * sinf(lean * DEG)),
               (double)angle, steps, (double)steps / SIM_HZ);
    }

    // 2. Rotate a settled flat bed in the screen plane, upright and leaned back.
    //    Below the (effective) repose angle the surface stays put; above it,
    //    it avalanches until the surface is that angle from true horizontal.
    static const float rot_leans[] = { 0, 60 };
    for (unsigned li = 0; li < 2; li++) {
        for (int deg = 20; deg <= 70; deg += 10) {
            settled_bed();
            memcpy(before, sim.cell, sizeof(before));
            set_board(rot_leans[li], (float)deg);
            run(8000);
            float a = surface_angle();
            printf("lean %2.0f, rotate %2d deg: %5d grains moved, surface %5.1f deg -> effective repose %5.1f deg\n",
                   (double)rot_leans[li], deg, snapshot_moved(before), (double)a, (double)(deg - a));
        }
    }

    // 3. Lying flat with noise: count cells that changed.
    settled_bed();
    memcpy(before, sim.cell, sizeof(before));
    srand(1);
    for (int i = 0; i < 2000; i++) {
        float nx = ((rand() % 2001) - 1000) / 1000.0f * 0.03f;
        float ny = ((rand() % 2001) - 1000) / 1000.0f * 0.03f;
        sim_set_gravity(&sim, nx, ny, -1.0f);
        sim_step(&sim);
    }
    printf("flat + 0.03 g noise, 2000 steps: %d grains moved\n", snapshot_moved(before));

    // 4. Nearly flat: settle upright, lean back, then turn the board 90 deg in
    //    its plane. Shows the lean at which glass friction holds the sand.
    for (int lean = 70; lean <= 86; lean += 2) {
        settled_bed();
        memcpy(before, sim.cell, sizeof(before));
        set_board((float)lean, 90);
        run(2000);
        printf("lean %2d deg (pull %.3f g vs glass %.3f g): %d grains moved\n", lean,
               (double)cosf(lean * DEG), (double)(BACK_FRICTION * sinf(lean * DEG)), snapshot_moved(before));
    }

    // 5. Tip past flat, top edge down (lean 120 = 30 deg past flat): sand
    //    should run to the top of the screen.
    settled_bed();
    float y0 = mean_y();
    set_board(120, 0);
    run(3000);
    printf("tipped past flat (lean 120): mean grain y %.1f -> %.1f (0 = top, %d = bottom)\n",
           (double)y0, (double)mean_y(), GRID_H);

    // 6. Shake a settled bed and check that it resettles.
    settled_bed();
    sim_shake(&sim, 0.0f, -1.0f, 1.5f);
    run(30);
    float m_shaken = sim.motion;
    run(1500);
    printf("shake: motion %.1f right after, %.2f after 1500 steps\n", (double)m_shaken, (double)sim.motion);

    // 7. Throughput with everything in motion.
    settled_bed();
    sim_set_gravity(&sim, 0.7f, 0.7f, 0);
    clock_t t0 = clock();
    run(1000);
    double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / 1000;
    printf("%d grains: %.0f us/step on host\n", sim.n, us);
    return 0;
}
