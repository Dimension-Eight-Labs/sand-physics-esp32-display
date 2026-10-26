// Host tests for how liquid moves: free fall, droplets landing on a pool,
// continuous pours, sloshing, knock splash, flip (no ESP-IDF needed). name=value
// arguments override liquid parameters (tools/overrides.h).
//
//   cc -std=c11 -O2 -Isrc src/sim.c src/material.c tools/sim_liquidfx.c -lm -o sim_liquidfx && ./sim_liquidfx
//
// Board upright unless stated; gravity in the screen frame (x right, y down).
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
static material_t wm; // liquid, with optional command-line overrides

static void step(void)
{
    sim_step(&sim);
}

// Splash events: knocks, and grains thrown by them.
static uint32_t splash_events(void)
{
    return sim.n_knocks;
}

// Longest run of grains stacked in one column with nothing under the run
// (a vertical line of airborne grains).
static int longest_air_line(void)
{
    int best = 0;
    for (int x = 0; x < GRID_W; x++) {
        int run = 0;
        for (int y = 0; y < GRID_H; y++) {
            if (sim.cell[y * GRID_W + x]) {
                run++;
            } else {
                if (run > best && y < GRID_H) {
                    best = run; // the run ended above an empty cell: detached
                }
                run = 0;
            }
        }
    }
    return best;
}

static int spray_count(void)
{
    int c = 0;
    for (int k = 0; k < sim.n; k++) {
        c += (sim.ground[k] & LIQ_SPRAY) != 0;
    }
    return c;
}

// Grains with no grain in any of the 4 cells around them (loose droplets).
static int loose_count(void)
{
    int c = 0;
    for (int k = 0; k < sim.n; k++) {
        int x = (int)sim.grains[k].x, y = (int)sim.grains[k].y, idx = y * GRID_W + x;
        c += !((x > 0 && sim.cell[idx - 1]) || (x < GRID_W - 1 && sim.cell[idx + 1]) ||
               (y > 0 && sim.cell[idx - GRID_W]) || (y < GRID_H - 1 && sim.cell[idx + GRID_W]));
    }
    return c;
}

static void pool(uint32_t seed)
{
    int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, seed);
    sim_set_material(&sim, &wm);
    sim_spawn(&sim, SPAWN_REGION_FRACTION, 4);
    sim_set_gravity(&sim, 0, 1, 0);
    for (int i = 0; i < 3000; i++) {
        step();
    }
}

static void rotate(float deg)
{
    sim_set_gravity(&sim, sinf(deg * DEG), cosf(deg * DEG), 0);
}

// Fitted angle (deg) of the column tops, ignoring airborne grains: a column's
// top is its first grain with another grain directly below it (or the floor).
static float surface_angle(void)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    for (int x = 0; x < GRID_W; x++) {
        for (int y = 0; y < GRID_H; y++) {
            if (sim.cell[y * GRID_W + x] && (y == GRID_H - 1 || sim.cell[(y + 1) * GRID_W + x])) {
                double hgt = GRID_H - y;
                sx += x;
                sy += hgt;
                sxx += (double)x * x;
                sxy += (double)x * hgt;
                cnt++;
                break;
            }
        }
    }
    if (cnt < 3) {
        return 0.0f;
    }
    return (float)(atan((cnt * sxy - sx * sy) / (cnt * sxx - sx * sx)) * 180.0 / 3.14159265);
}

// Highest row (smallest y) of the body's surface, i.e. ignoring droplets.
static int body_top_row(void)
{
    int best = GRID_H;
    for (int x = 0; x < GRID_W; x++) {
        for (int y = 0; y < GRID_H; y++) {
            if (sim.cell[y * GRID_W + x] && (y == GRID_H - 1 || sim.cell[(y + 1) * GRID_W + x])) {
                best = y < best ? y : best;
                break;
            }
        }
    }
    return best;
}

// 1. Flip a settled pool upside down and watch the fall.
static void test_fall(void)
{
    pool(1);
    rotate(180);
    uint32_t p0 = sim.n_pushes;
    double vx_sum = 0;
    float vx_max = 0;
    long samples = 0;
    // While falling (first 0.6 s): grains moving "down" (toward y = 0) faster than 0.2.
    for (int i = 0; i < SIM_HZ * 6 / 10; i++) {
        step();
        for (int k = 0; k < sim.n; k++) {
            const sim_grain_t *g = &sim.grains[k];
            if (g->vy < -0.2f) {
                float a = fabsf(g->vx);
                vx_sum += (double)a;
                vx_max = a > vx_max ? a : vx_max;
                samples++;
            }
        }
    }
    printf("free fall (pool flipped): %u spread pushes in the first 0.6 s; falling grains' sideways speed "
           "mean %.3f, max %.2f cells/step (fall speed up to %.1f)\n",
           sim.n_pushes - p0, samples ? vx_sum / (double)samples : 0.0, (double)vx_max,
           (double)material_get(MATERIAL_LIQUID)->max_speed);
}

// Drop `count` grains taken from the surface of a settled pool from
// `height` cells above it (staggered by `stagger` cells each), then watch the
// pool's total motion until it is back at rest.
static void test_drops(int count, int height, int stagger, uint32_t seed)
{
    pool(seed);
    const int surface = body_top_row();
    uint32_t s0 = splash_events(), sl0 = sim.n_spray_landings;
    // Take top grains of evenly spaced columns and put them in the air above.
    int moved = 0;
    for (int c = 0; c < count; c++) {
        int x = (c * 2 + 1) * GRID_W / (2 * count);
        for (int y = 0; y < GRID_H; y++) {
            int id = sim.cell[y * GRID_W + x];
            if (!id) {
                continue;
            }
            int ty = surface - height - c * stagger;
            if (ty < 0 || sim.cell[ty * GRID_W + x]) {
                break;
            }
            sim.cell[y * GRID_W + x] = 0;
            sim.cell[ty * GRID_W + x] = (uint16_t)id;
            sim_grain_t *g = &sim.grains[id - 1];
            g->x = x + 0.5f;
            g->y = ty + 0.5f;
            g->vx = 0.0f;
            g->vy = 0.0f;
            moved++;
            break;
        }
    }
    sim_shake(&sim, 0.0f, 0.0f, 0.0f); // wake everything, no push
    // Last landing: the step after which no grain is above the body any more.
    static float mot[SIM_HZ * 20];
    const int steps = SIM_HZ * 20;
    int landed = -1, rest = -1;
    for (int i = 0; i < steps; i++) {
        step();
        mot[i] = sim.motion;
        int air = 0;
        for (int k = 0; k < sim.n; k++) {
            air += sim.grains[k].y < (float)(surface - 1);
        }
        if (air) {
            landed = -1;
        } else if (landed < 0) {
            landed = i;
        }
        if (rest < 0 && landed >= 0 && sim.motion == 0.0f && sim.awake < 200) {
            rest = i;
        }
    }
    // Monotonic decay after the last landing: per step, and over 5-step sums
    // (the motion of a few grains moving one cell is lumpy by nature).
    int rises = 0, rises5 = 0;
    float peak_after = 0.0f, rise_max = 0.0f;
    if (landed >= 0) {
        for (int i = landed + 1; i < steps; i++) {
            float d = mot[i] - mot[i - 1];
            if (d > 1e-4f) {
                rises++;
                rise_max = d > rise_max ? d : rise_max;
            }
            peak_after = mot[i] > peak_after ? mot[i] : peak_after;
        }
        for (int i = landed + 10; i + 5 <= steps; i += 5) {
            float a = 0, b = 0;
            for (int k = 0; k < 5; k++) {
                a += mot[i - 5 + k];
                b += mot[i + k];
            }
            rises5 += b > a + 1e-4f;
        }
    }
    printf("drops: %d grains from %d..%d cells above a settled pool: last lands at %.2f s (motion %.2f); after "
           "that motion rises on %d steps (largest rise %.2f, peak %.2f) and on %d of the 5-step sums; "
           "%u splash events, %u spray landings; at rest (motion 0, < 200 awake) at %.2f s, %d awake at the end\n",
           moved, height, height + (count - 1) * stagger, landed < 0 ? -1.0 : (double)landed / SIM_HZ,
           landed < 0 ? 0.0 : (double)mot[landed], rises, (double)rise_max, (double)peak_after, rises5,
           splash_events() - s0, sim.n_spray_landings - sl0, rest < 0 ? -1.0 : (double)rest / SIM_HZ, sim.awake);
}

// Height (rows from the floor) of the body in column x: the run of grains
// packed against the floor.
static int column_height(int x)
{
    int y = GRID_H - 1;
    while (y >= 0 && sim.cell[y * GRID_W + x]) {
        y--;
    }
    return GRID_H - 1 - y;
}

// Continuous pour: a 2-wide stream from the top onto a settled pool for 4 s.
// on_wave: tilt 25 deg for 1.5 s first and start pouring onto the high side
// the moment the board comes back upright (a tall, moving wave section and
// its slope). Reports the highest any grain near the stream rises above the
// local surface, the mound under the stream, and splash events. stream 0:
// the same run without pouring (baseline for the wave's own shape).
static void test_pour(int on_wave, int stream, uint32_t seed)
{
    const int cap = (int)(FILL_FRACTION * GRID_W * GRID_H);
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, cap));
    sim_init(&sim, mem, GRID_W, GRID_H, cap, seed);
    sim_set_material(&sim, &wm);
    sim.n = 0;
    const int pool_n = cap - 2 * 4 * SIM_HZ - 40;
    for (int y = GRID_H - 1; y >= 0 && sim.n < pool_n; y--) {
        for (int x = 0; x < GRID_W && sim.n < pool_n; x++) {
            sim_grain_t *g = &sim.grains[sim.n];
            g->x = x + 0.5f;
            g->y = y + 0.5f;
            g->vx = g->vy = 0.0f;
            sim.cell[y * GRID_W + x] = (uint16_t)(sim.n + 1);
            sim.n++;
        }
    }
    sim_set_gravity(&sim, 0, 1, 0);
    for (int i = 0; i < 1500; i++) {
        step();
    }
    int src = GRID_W / 2;
    if (on_wave) {
        rotate(25);
        for (int i = 0; i < SIM_HZ * 3 / 2; i++) {
            step();
        }
        rotate(0);
        src = GRID_W * 3 / 4; // the high side
    }
    const uint32_t s0 = splash_events(), l0 = sim.n_landings;
    int rise_max = 0, risers_max = 0, mound_max = 0, dip_max = 0, side_max_x = 0;
    float side_v = 0.0f;
    for (int i = 0; i < SIM_HZ * 6; i++) {
        if (stream && i < SIM_HZ * 4) {
            for (int x = src; x <= src + 1; x++) {
                if (!sim.cell[GRID_W + x] && sim.n < cap) {
                    sim_grain_t *g = &sim.grains[sim.n];
                    g->x = x + 0.5f;
                    g->y = 1.5f;
                    g->vx = 0.0f;
                    g->vy = 0.5f;
                    sim.idle[sim.n] = 0;
                    sim.ground[sim.n] = 0;
                    sim.cell[GRID_W + x] = (uint16_t)(sim.n + 1);
                    sim.n++;
                }
            }
        }
        step();
        // Local surface: median body height of the columns 3..12 cells either
        // side of the stream.
        int hs[20], nh = 0;
        for (int d = 3; d <= 12; d++) {
            if (src - d + 1 >= 0) {
                hs[nh++] = column_height(src - d + 1);
            }
            if (src + d < GRID_W) {
                hs[nh++] = column_height(src + d);
            }
        }
        for (int a = 1; a < nh; a++) {
            for (int b = a; b > 0 && hs[b - 1] > hs[b]; b--) {
                int t = hs[b];
                hs[b] = hs[b - 1];
                hs[b - 1] = t;
            }
        }
        const int surf = hs[nh / 2];
        const int surf_y = GRID_H - surf; // first row above the surface
        // Mound under the stream: columns right beside it.
        for (int x = src - 2; x <= src + 3; x++) {
            if (x >= 0 && x < GRID_W) {
                int m = column_height(x) - surf;
                mound_max = m > mound_max ? m : mound_max;
                dip_max = -m > dip_max ? -m : dip_max;
            }
        }
        // Grains near the impact moving up (against gravity) above the surface.
        int risers = 0;
        for (int k = 0; k < sim.n; k++) {
            const sim_grain_t *g = &sim.grains[k];
            const int x = (int)g->x, y = (int)g->y;
            if (x < src - 12 || x > src + 13 || y >= surf_y) {
                continue;
            }
            if (g->vy < -0.05f) {
                risers++;
                rise_max = surf_y - y > rise_max ? surf_y - y : rise_max;
            }
            if (fabsf(g->vx) > side_v && (x < src || x > src + 1)) {
                side_v = fabsf(g->vx);
                side_max_x = x - src;
            }
        }
        risers_max = risers > risers_max ? risers : risers_max;
    }
    printf("%s %s: 2-wide stream for 4 s (%u landings absorbed): grains near the impact rise at most %d "
           "cells above the local surface (%d rising at once at most); beside the stream up to %d cells above / %d below the local surface; "
           "fastest sideways grain above the surface %.2f cells/step (%+d cells from the stream); %u splash events; wave backstop %u\n",
           stream ? "pour onto" : "NO pour (baseline), same window on", on_wave ? "the high side of a returning 25 deg wave" : "a settled pool", sim.n_landings - l0, rise_max,
           risers_max, mound_max, dip_max, (double)side_v, side_max_x, splash_events() - s0, sim.n_wave_backstop);
}

// 2. Tilt, hold, return; track the surface angle until it settles.
static void test_slosh(float tilt)
{
    pool(2);
    rotate(tilt);
    for (int i = 0; i < SIM_HZ * 3 / 2; i++) {
        step();
    }
    const float held = surface_angle();
    const uint32_t s0 = splash_events();
    rotate(0);
    float prev_sign = 0, peak = 0;
    int crossings = 0, last_big = 0;
    float peaks[16];
    int np = 0;
    const int steps = SIM_HZ * 8;
    for (int i = 0; i < steps; i++) {
        step();
        // Smoothed over the last 6 steps (50 ms): while the body is moving
        // the fitted angle of single steps jumps by several degrees.
        static float hist[6];
        hist[i % 6] = surface_angle();
        float a = 0.0f;
        for (int k = 0; k < 6; k++) {
            a += hist[i < 6 ? i % 6 : k] / 6.0f;
        }
        if (fabsf(a) > 1.0f) {
            float sg = a > 0 ? 1.0f : -1.0f;
            if (prev_sign != 0 && sg != prev_sign) {
                crossings++;
                if (np < 16) {
                    peaks[np++] = peak;
                }
                peak = 0;
            }
            prev_sign = sg;
            peak = fabsf(a) > fabsf(peak) ? a : peak;
        }
        if (i == steps - 1 && np < 16 && peak != 0) {
            peaks[np++] = peak; // the last swing
        }
        if (fabsf(a) > 0.7f || sim.motion > 5.0f) {
            last_big = i;
        }
    }
    printf("tilt %2.0f deg (surface reached %.1f deg in 1.5 s) and back: %d swings past level (|surface, 50 ms mean| > 1 deg each side)", (double)tilt, (double)held, crossings);
    if (np) {
        printf(", peaks");
        for (int k = 0; k < np && k < 6; k++) {
            printf(" %+.1f", (double)peaks[k]);
        }
        printf(" deg");
    }
    printf("; settled (|surface| <= 0.7 deg, motion <= 5) after %.2f s; %u splash events; wave backstop %u\n", (double)last_big / SIM_HZ,
           splash_events() - s0, sim.n_wave_backstop);
}

// 3. A knock: a few steps of shake impulses, as the IMU delivers them.
static void test_knock(void)
{
    pool(3);
    const int surface = body_top_row();
    const uint32_t k0 = sim.n_knocks, g0 = sim.n_knock_grains, sl0 = sim.n_spray_landings;
    float profile[] = { 1.0f, 0.8f, 0.5f, 0.3f };
    // Launch directions of the grains each impulse throws, relative to "up"
    // (degrees, + = toward the knock), and launch speeds.
    static uint8_t was[8192];
    double amin = 1e9, amax = -1e9, asum = 0, asq = 0, vmin = 1e9, vmax = 0;
    int nl = 0, straight = 0;
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < sim.n; k++) {
            was[k] = sim.ground[k] & LIQ_SPRAY;
        }
        sim_shake(&sim, 1.0f, 0.0f, SHAKE_STRENGTH * profile[i]); // knock pushing toward +x
        for (int k = 0; k < sim.n; k++) {
            if ((sim.ground[k] & LIQ_SPRAY) && !was[k]) {
                const sim_grain_t *g = &sim.grains[k];
                double a = atan2(g->vx, -g->vy) * 180.0 / 3.14159265;
                double v = sqrt(g->vx * g->vx + g->vy * g->vy);
                amin = a < amin ? a : amin;
                amax = a > amax ? a : amax;
                asum += a;
                asq += a * a;
                vmin = v < vmin ? v : vmin;
                vmax = v > vmax ? v : vmax;
                straight += fabs(a) < 5.0;
                nl++;
            }
        }
        step();
    }
    int peak = 0, airborne_peak = 0, land_step = -1, absorbed = -1, line_peak = 0;
    for (int i = 0; i < SIM_HZ * 6; i++) {
        step();
        if (absorbed < 0 && spray_count() == 0) {
            absorbed = i;
        }
        // In the air: nothing directly below it (droplets, not the sloshing body).
        int air = 0;
        for (int k = 0; k < sim.n; k++) {
            int x = (int)sim.grains[k].x, y = (int)sim.grains[k].y;
            if (y < GRID_H - 1 && !sim.cell[(y + 1) * GRID_W + x]) {
                air++;
                peak = surface - y > peak ? surface - y : peak;
            }
        }
        airborne_peak = air > airborne_peak ? air : airborne_peak;
        int line = longest_air_line();
        line_peak = line > line_peak ? line : line_peak;
        if (land_step < 0 && i > 5 && air == 0) {
            land_step = i;
        }
    }
    const double mean = nl ? asum / nl : 0, sd = nl ? sqrt(asq / nl - mean * mean) : 0;
    printf("knock (1 g, 4 impulses): %u grains thrown; launch angle from up %+.0f..%+.0f deg (mean %+.0f, sd %.0f; "
           "%d within 5 deg of straight up), speed %.2f..%.2f cells/step\n",
           sim.n_knock_grains - g0, amin, amax, mean, sd, straight, vmin, vmax);
    printf("  droplets up to %d cells above the resting surface (pool depth %d), %d grains in the air at most, "
           "longest vertical run of airborne grains %d; all down after %.2f s; all spray absorbed and at rest after "
           "%.2f s\n",
           peak, GRID_H - surface, airborne_peak, line_peak, land_step < 0 ? -1.0 : (double)(land_step + 4) / SIM_HZ,
           absorbed < 0 ? -1.0 : (double)(absorbed + 4) / SIM_HZ);
    printf("  splash events: %u (spray landings absorbed: %u)\n", sim.n_knocks - k0, sim.n_spray_landings - sl0);
}

// 4. Flip a settled pool: breakup during the fall, impact droplets, settling.
static void test_flip(void)
{
    pool(4);
    const int depth = GRID_H - body_top_row();
    rotate(180);
    printf("flip (pool %d deep falls %d cells):\n", depth, GRID_H - depth);
    const uint32_t s0 = splash_events(), sl0 = sim.n_spray_landings, l0 = sim.n_landings;
    int loose_peak = 0;
    int landed_at = -1, splash_peak = 0, level_at = -1, droplets_peak = 0;
    static int hist[GRID_H];
    for (int i = 1; i <= SIM_HZ * 6; i++) {
        step();
        if (landed_at < 0) {
            int l = loose_count();
            loose_peak = l > loose_peak ? l : loose_peak;
        }
        // Body extent and how broken up it is: rows inside it that are < 60 % full.
        int y0 = GRID_H, y1 = -1;
        static int rowfill[GRID_H];
        memset(rowfill, 0, sizeof(rowfill));
        for (int k = 0; k < sim.n; k++) {
            int y = (int)sim.grains[k].y;
            rowfill[y]++;
            y0 = y < y0 ? y : y0;
            y1 = y > y1 ? y : y1;
        }
        int sparse = 0;
        for (int y = y0; y <= y1; y++) {
            sparse += rowfill[y] < GRID_W * 6 / 10;
        }
        if (landed_at < 0 && rowfill[0] > GRID_W / 2) {
            landed_at = i;
        }
        // After landing: droplets = grains thrown back away from the new floor
        // (moving toward larger y), measured above the new surface.
        if (landed_at > 0 && i < landed_at + 2 * SIM_HZ) {
            // Droplets: grains moving away from the new floor more than 2 cells
            // above where its level surface will be (pool depth). Report the
            // height reached by the 5th-highest, so one stray does not decide it.
            const int surf = depth;
            memset(hist, 0, sizeof(hist));
            int nd = 0;
            for (int k = 0; k < sim.n; k++) {
                int hgt = (int)sim.grains[k].y - surf;
                if (sim.grains[k].vy > 0.2f && hgt > 2) {
                    hist[hgt]++;
                    nd++;
                }
            }
            droplets_peak = nd > droplets_peak ? nd : droplets_peak;
            int seen = 0;
            for (int hgt = GRID_H - 1; hgt >= 0; hgt--) {
                seen += hist[hgt];
                if (seen >= 5) {
                    splash_peak = hgt > splash_peak ? hgt : splash_peak;
                    break;
                }
            }
        }
        // Level: the bottom of the inverted pool (largest y per column) within 2 cells.
        if (landed_at > 0 && level_at < 0 && i % 6 == 0) {
            int lo = GRID_H, hi = -1;
            for (int x = 0; x < GRID_W; x++) {
                int yy = -1;
                for (int y = 0; y < GRID_H; y++) {
                    if (sim.cell[y * GRID_W + x] && (y == 0 || sim.cell[(y - 1) * GRID_W + x])) {
                        yy = y;
                    }
                }
                lo = yy < lo ? yy : lo;
                hi = yy > hi ? yy : hi;
            }
            if (hi - lo <= 2 && sim.motion < 20.0f) {
                level_at = i;
            }
        }
        if (i == SIM_HZ / 10 || i == SIM_HZ * 2 / 10 || i == SIM_HZ * 4 / 10 || i == SIM_HZ * 6 / 10) {
            int falling = 0;
            for (int k = 0; k < sim.n; k++) {
                falling += sim.grains[k].vy < -0.2f;
            }
            printf("  t=%.1f s: %3d%% of grains falling; body spans rows %d..%d (%d tall, was %d), %d of those "
                   "rows < 60%% full; %d loose grains\n",
                   (double)i / SIM_HZ, falling * 100 / sim.n, y0, y1, y1 - y0 + 1, depth, sparse, loose_count());
        }
    }
    printf("  lands at %.2f s; up to %d grains moving back up more than 2 cells above the level surface, "
           "5th highest %d cells; level (<= 2 cells, motion < 20) at %.2f s\n",
           (double)landed_at / SIM_HZ, droplets_peak, splash_peak, level_at < 0 ? -1.0 : (double)level_at / SIM_HZ);
    printf("  fall: at most %d loose grains (no neighbour) before landing; %u landings absorbed; %u splash "
           "events; wave backstop %u\n",
           loose_peak, sim.n_landings - l0, splash_events() - s0, sim.n_wave_backstop);
    (void)sl0;
}

// 5. A gravel heap switched to liquid: how fast it slumps flat.
static void test_heap(void)
{
    int n = (int)(FILL_FRACTION * GRID_W * GRID_H);
    free(mem);
    mem = malloc(sim_mem_bytes(GRID_W, GRID_H, n));
    sim_init(&sim, mem, GRID_W, GRID_H, n, 21);
    sim.n = 0;
    sim_set_gravity(&sim, 0, 1, 0);
    while (sim.n < 1800) {
        for (int k = -1; k <= 1 && sim.n < 1800; k++) {
            int x = GRID_W / 2 + k;
            if (sim.cell[GRID_W + x] == 0) {
                sim_grain_t *g = &sim.grains[sim.n];
                g->x = x + 0.5f;
                g->y = 1.5f;
                g->vx = g->vy = 0;
                sim.cell[GRID_W + x] = (uint16_t)(sim.n + 1);
                sim.n++;
            }
        }
        step();
    }
    for (int i = 0; i < 2000; i++) {
        step();
    }
    sim_set_material(&sim, &wm);
    float t_level = -1;
    for (int i = 1; i <= SIM_HZ * 5; i++) {
        step();
        if (i % 6 == 0 && t_level < 0) {
            int lo = GRID_H, hi = 0;
            for (int x = 0; x < GRID_W; x++) {
                int h = 0;
                for (int y = 0; y < GRID_H; y++) {
                    if (sim.cell[y * GRID_W + x] && (y == GRID_H - 1 || sim.cell[(y + 1) * GRID_W + x])) {
                        h = GRID_H - y;
                        break;
                    }
                }
                lo = h < lo ? h : lo;
                hi = h > hi ? h : hi;
            }
            if (hi - lo <= 2) {
                t_level = (float)i / SIM_HZ;
            }
        }
    }
    printf("gravel heap (1800 grains) switched to liquid: within 2 cells of level after %.2f s\n", (double)t_level);
}

int main(int argc, char **argv)
{
    wm = *material_get(MATERIAL_LIQUID);
    for (int a = 1; a < argc; a++) {
        override_apply(&wm, argv[a]);
    }
    const material_t *w = &wm;
    printf("liquid: viscosity %.2f, impact_absorb %.2f, impact_to_flow %.2f, knock %.2f (fan +/-%.0f deg, top %d, "
           "max %d, interior %.2f), wave gain %.3f damping %.3f\n",
           (double)w->viscosity, (double)w->impact_absorb, (double)w->impact_to_flow, (double)w->knock_splash,
           (double)w->knock_fan_deg, w->knock_depth, w->knock_max, (double)w->knock_interior, (double)w->wave_gain,
           (double)w->wave_damping);
    override_print(material_get(MATERIAL_LIQUID), w);
    test_fall();
    test_drops(5, 20, 0, 5);
    test_drops(5, 40, 7, 6);
    test_drops(20, 10, 3, 7);
    test_pour(0, 1, 8);
    test_pour(1, 1, 9);
    test_pour(1, 0, 9);
    test_slosh(10.0f);
    test_slosh(25.0f);
    test_knock();
    test_flip();
    test_heap();
    free(mem);
    return 0;
}
