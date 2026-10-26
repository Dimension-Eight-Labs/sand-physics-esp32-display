// Falling-sand simulation. See docs/SPEC.md §5 for the model.
//
// Grains integrate float velocity under gravity and move cell to cell on an
// occupancy grid (the PixelDust idea). On top of that, grains resting on the
// pile have static friction and only move through the repose rule, which
// compares the local surface slope against the angle of repose.
#include "sim.h"

#include <math.h>
#include <string.h>

#include "config.h"

#define DEG2RAD (3.14159265f / 180.0f)

// The 8 grid directions, index k at angle k*45° in screen coordinates
// (x right, y down): 0 = +x, 2 = +y (down the screen), 4 = -x, 6 = -y.
static const int8_t DIR_X[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int8_t DIR_Y[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

// ---- RNG (xorshift32) ------------------------------------------------------

static inline uint32_t rng_next(sim_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

// Uniform in [0, 1).
static inline float rng_unit(sim_t *s)
{
    return (float)(rng_next(s) >> 8) * (1.0f / 16777216.0f);
}

// Uniform in [-1, 1).
static inline float rng_signed(sim_t *s)
{
    return rng_unit(s) * 2.0f - 1.0f;
}

// ---- Fast single-precision math ------------------------------------------------
//
// The ESP32-S3 FPU does add/multiply in hardware but not divide or square
// root; sqrtf(), floorf(), ceilf() and '/' become software routines costing
// hundreds of cycles. The per-grain paths below only use these helpers.

// floor() for the small coordinates used here (works for negatives too).
static inline int ifloor(float x)
{
    int i = (int)x;
    return i - (x < (float)i);
}

// 1/sqrt(x) for x > 0: bit-level first guess plus one Newton step (~0.2 %).
static inline float rsqrt(float x)
{
    union {
        float f;
        uint32_t u;
    } v = { .f = x };
    v.u = 0x5f375a86u - (v.u >> 1);
    return v.f * (1.5f - 0.5f * x * v.f * v.f);
}

// 1/n for the sub-step count (n <= SIM_MAX_SUBSTEPS).
#define SIM_MAX_SUBSTEPS 8
static const float INV_N[SIM_MAX_SUBSTEPS + 1] = {
    1.0f, 1.0f, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8,
};
_Static_assert(MAX_SPEED <= (float)SIM_MAX_SUBSTEPS, "MAX_SPEED needs more sub-steps");

// ---- Grid helpers -------------------------------------------------------------

static inline int in_bounds(const sim_t *s, int x, int y)
{
    return x >= 0 && y >= 0 && x < s->w && y < s->h;
}

// Free = inside the grid and not occupied. Walls count as blocked.
static inline int is_free(const sim_t *s, int x, int y)
{
    return in_bounds(s, x, y) && s->cell[y * s->w + x] == 0;
}

static inline void wake_all(sim_t *s)
{
    memset(s->idle, 0, (size_t)s->n);
    s->sleeping = 0;
}

// A grain left (x, y): wake sleepers around it, since the hole may let them
// fall or slide. Further-away effects propagate as those grains move.
static void wake_around(sim_t *s, int x, int y)
{
    if (s->sleeping == 0) {
        return; // common while everything is in motion: nothing to wake
    }
    for (int dy = -WAKE_RADIUS; dy <= WAKE_RADIUS; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= s->h) {
            continue;
        }
        const uint16_t *row = &s->cell[yy * s->w];
        for (int dx = -WAKE_RADIUS; dx <= WAKE_RADIUS; dx++) {
            int xx = x + dx;
            if (xx >= 0 && xx < s->w && row[xx]) {
                uint8_t *idle = &s->idle[row[xx] - 1];
                s->sleeping -= *idle >= SLEEP_STEPS;
                *idle = 0;
            }
        }
    }
}

static inline void cell_move(sim_t *s, int from_x, int from_y, int to_x, int to_y)
{
    uint16_t id = s->cell[from_y * s->w + from_x];
    s->cell[from_y * s->w + from_x] = 0;
    s->cell[to_y * s->w + to_x] = id;
}

// ---- Setup --------------------------------------------------------------------

size_t sim_mem_bytes(int w, int h, int n)
{
    size_t grains = (size_t)n * sizeof(sim_grain_t);
    size_t cells = (size_t)w * h * sizeof(uint16_t);
    size_t bytes = ((size_t)n + 3) & ~(size_t)3; // shade, then idle
    return grains + cells + 2 * bytes;
}

void sim_init(sim_t *s, void *mem, int w, int h, int n, uint32_t seed)
{
    memset(s, 0, sizeof(*s));
    s->w = w;
    s->h = h;
    s->n = n;

    // Layout: grains (float-aligned), cells (uint16), shades, idle counters.
    uint8_t *p = mem;
    s->grains = (sim_grain_t *)p;
    p += (size_t)n * sizeof(sim_grain_t);
    s->cell = (uint16_t *)p;
    p += (size_t)w * h * sizeof(uint16_t);
    s->shade = p;
    p += ((size_t)n + 3) & ~(size_t)3;
    s->idle = p;
    memset(s->idle, 0, (size_t)n);

    s->rng = seed ? seed : 0x9E3779B9u;
    s->down = -1;
    memset(s->cell, 0, (size_t)w * h * sizeof(uint16_t));
}

void sim_spawn(sim_t *s, float region, int n_shades)
{
    memset(s->cell, 0, (size_t)s->w * s->h * sizeof(uint16_t));
    wake_all(s);

    int rows = (int)(region * (float)s->h + 0.5f);
    if (rows < 1) {
        rows = 1;
    }
    // Make sure the region can hold every grain.
    int min_rows = (s->n + s->w - 1) / s->w;
    if (rows < min_rows) {
        rows = min_rows;
    }
    if (rows > s->h) {
        rows = s->h;
    }
    int region_cells = rows * s->w;

    for (int i = 0; i < s->n; i++) {
        // Random probing first, then a linear scan from the last probe so a
        // nearly full region still terminates quickly.
        int idx = (int)(rng_next(s) % (uint32_t)region_cells);
        for (int tries = 0; tries < 16 && s->cell[idx] != 0; tries++) {
            idx = (int)(rng_next(s) % (uint32_t)region_cells);
        }
        while (s->cell[idx] != 0) {
            idx = (idx + 1) % region_cells;
        }
        s->cell[idx] = (uint16_t)(i + 1);

        sim_grain_t *g = &s->grains[i];
        g->x = (float)(idx % s->w) + 0.25f + 0.5f * rng_unit(s);
        g->y = (float)(idx / s->w) + 0.25f + 0.5f * rng_unit(s);
        g->vx = 0.2f * rng_signed(s);
        g->vy = 0.2f * rng_signed(s);
        s->shade[i] = (uint8_t)(rng_next(s) % (uint32_t)n_shades);
    }
}

// ---- Inputs ------------------------------------------------------------------------

static void apply_gravity(sim_t *s, float gx, float gy, float gz)
{
    // The in-plane projection of gravity drives the sand as-is (not
    // normalised): upright = full pull, leaning back weakens it, past flat it
    // reverses. The screen-normal part presses grains against the glass.
    const float g_xy = sqrtf(gx * gx + gy * gy);
    const float glass = BACK_FRICTION * fabsf(gz); // friction, in g
    s->ax = gx * GRAVITY_SCALE;
    s->ay = gy * GRAVITY_SCALE;
    s->friction = glass * GRAVITY_SCALE;

    // The pile only "feels" a down direction when the in-plane pull beats the
    // static friction against the glass; otherwise it is frozen in place.
    const float net = g_xy - glass;
    if (net < GRAVITY_DEADZONE) {
        s->down = -1;
        s->repose_prob = 0.0f;
        return;
    }

    // Snap to the nearest of 8 directions, remembering the residual tilt.
    float ang = atan2f(gy, gx) / (45.0f * DEG2RAD); // in units of 45°
    float k = floorf(ang + 0.5f);
    float tilt_deg = (ang - k) * 45.0f;             // -22.5 .. 22.5, positive toward k+1
    s->down = ((int)k % 8 + 8) % 8;

    // Steepest stable slope. A grain on a slope θ is held by internal friction
    // (tan(repose) · g_xy·cosθ) plus glass friction (BACK_FRICTION · |gz|):
    //   tan θ_max = tan(repose) + glass / (g_xy · cos θ)
    // so piles stand steeper the further the board leans back.
    // Diagonal frames under-measure drops by about a cell, hence the correction.
    float base = REPOSE_ANGLE_DEG - ((s->down & 1) ? REPOSE_DIAG_CORRECTION_DEG : 0.0f);
    float tan_base = tanf(base * DEG2RAD) + glass / (g_xy * cosf(base * DEG2RAD));
    base = atanf(tan_base) / DEG2RAD;

    // Lateral side +1 is direction down+2 (90° toward k+1), side -1 is down-2.
    // Gravity leaning toward a side makes that side's slope effectively steeper.
    float thr_pos = fminf(base - tilt_deg, 89.0f);
    float thr_neg = fminf(base + tilt_deg, 89.0f);
    s->repose_tan[1] = tanf(thr_pos * DEG2RAD);
    s->repose_tan[0] = tanf(thr_neg * DEG2RAD);

    // Avalanches run slower under a weaker net pull (fall time ~ 1/sqrt(g)).
    s->repose_prob = REPOSE_PROB * fminf(1.0f, sqrtf(net));
}

void sim_set_gravity(sim_t *s, float gx, float gy, float gz)
{
    apply_gravity(s, gx, gy, gz);

    // Sleeping grains were stable under the gravity they fell asleep with.
    // Wake everyone when it changes meaningfully: another down direction, the
    // pile freezing/unfreezing, or the vector moving by WAKE_GRAVITY_DELTA_G.
    float dx = gx - s->ref_g[0], dy = gy - s->ref_g[1], dz = gz - s->ref_g[2];
    if (s->down != s->ref_down ||
        dx * dx + dy * dy + dz * dz > WAKE_GRAVITY_DELTA_G * WAKE_GRAVITY_DELTA_G) {
        wake_all(s);
        s->ref_g[0] = gx;
        s->ref_g[1] = gy;
        s->ref_g[2] = gz;
        s->ref_down = s->down;
    }
}

void sim_shake(sim_t *s, float dir_x, float dir_y, float strength)
{
    wake_all(s);
    float len = sqrtf(dir_x * dir_x + dir_y * dir_y);
    if (len > 1e-6f) {
        dir_x /= len;
        dir_y /= len;
    } else {
        dir_x = dir_y = 0.0f;
    }
    for (int i = 0; i < s->n; i++) {
        sim_grain_t *g = &s->grains[i];
        float d = strength * (0.5f + 0.5f * rng_unit(s));
        g->vx += dir_x * d + 0.5f * strength * rng_signed(s);
        g->vy += dir_y * d + 0.5f * strength * rng_signed(s);
    }
}

// ---- Step --------------------------------------------------------------------------

// Repose rule for a resting grain at (cx, cy). Measures how far the surface
// drops on each lateral side and slides the grain one cell if the slope is
// steeper than the (tilt-corrected) angle of repose. Returns 1 if it moved.
static int try_repose(sim_t *s, sim_grain_t *g, int cx, int cy)
{
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    int first = (int)(rng_next(s) & 1);

    for (int t = 0; t < 2; t++) {
        int side = first ^ t;                        // 0 => side -1, 1 => side +1
        int lat = (s->down + (side ? 2 : 6)) & 7;
        const int sx = DIR_X[lat], sy = DIR_Y[lat];
        // Dithering the threshold lets the average slope fall between the
        // few slopes a small grid can represent (2/3, 3/4, 1, ...).
        const float tan_thr = s->repose_tan[side] * (1.0f + REPOSE_DITHER * rng_signed(s));

        int slide = 0;
        for (int k = 1; k <= REPOSE_SCAN && !slide; k++) {
            int px = cx + k * sx, py = cy + k * sy;
            if (!is_free(s, px, py)) {
                break; // surface rises or wall: nothing further out matters
            }
            int drop = 0;
            while (drop < REPOSE_SCAN && is_free(s, px + (drop + 1) * dx, py + (drop + 1) * dy)) {
                drop++;
            }
            if ((float)drop > (float)k * tan_thr) {
                slide = 1;
            }
        }
        if (!slide) {
            continue;
        }

        int tx, ty;
        if (is_free(s, cx + sx + dx, cy + sy + dy)) {
            tx = cx + sx + dx; // slide down the slope
            ty = cy + sy + dy;
        } else {
            tx = cx + sx;      // creep sideways; it will drop next step
            ty = cy + sy;
        }
        cell_move(s, cx, cy, tx, ty);
        g->x += (float)(tx - cx);
        g->y += (float)(ty - cy);
        // A token velocity so the renderer shows the slide; it stays below
        // REST_SPEED, so a supported grain is back at rest next step.
        g->vx = (float)(tx - cx) * (0.8f * REST_SPEED) * 0.7071f;
        g->vy = (float)(ty - cy) * (0.8f * REST_SPEED) * 0.7071f;
        return 1;
    }
    return 0;
}

// Move a grain along its velocity in sub-steps of at most one cell, resolving
// collisions axis by axis.
static void move_grain(sim_t *s, sim_grain_t *g, int cx, int cy)
{
    float ax = fabsf(g->vx), ay = fabsf(g->vy);
    float m = ax > ay ? ax : ay;
    if (m <= 0.0f) {
        return;
    }
    int n = (int)m;
    n += (float)n < m; // ceil
    if (n > SIM_MAX_SUBSTEPS) {
        n = SIM_MAX_SUBSTEPS;
    }
    const float inv_n = INV_N[n];

    for (int i = 0; i < n; i++) {
        float nx = g->x + g->vx * inv_n;
        float ny = g->y + g->vy * inv_n;
        int ncx = ifloor(nx);
        int ncy = ifloor(ny);

        if (ncx == cx && ncy == cy) {
            g->x = nx;
            g->y = ny;
            continue;
        }
        if (is_free(s, ncx, ncy)) {
            cell_move(s, cx, cy, ncx, ncy);
            g->x = nx;
            g->y = ny;
            cx = ncx;
            cy = ncy;
            continue;
        }

        int move_x = ncx != cx;
        int move_y = ncy != cy;
        if (move_x && move_y) {
            // Diagonal target blocked: try each axis alone, faster one first.
            int x_first = fabsf(g->vx) >= fabsf(g->vy);
            int done = 0;
            for (int a = 0; a < 2 && !done; a++) {
                int try_x = (a == 0) == x_first;
                if (try_x && is_free(s, ncx, cy)) {
                    cell_move(s, cx, cy, ncx, cy);
                    cx = ncx;
                    g->x = nx;
                    g->vy = -g->vy * ELASTICITY;
                    done = 1;
                } else if (!try_x && is_free(s, cx, ncy)) {
                    cell_move(s, cx, cy, cx, ncy);
                    cy = ncy;
                    g->y = ny;
                    g->vx = -g->vx * ELASTICITY;
                    done = 1;
                }
            }
            if (!done) {
                g->vx = -g->vx * ELASTICITY;
                g->vy = -g->vy * ELASTICITY;
                return;
            }
        } else if (move_x) {
            g->vx = -g->vx * ELASTICITY; // x blocked, y stays inside this cell
            g->y = ny;
        } else {
            g->vy = -g->vy * ELASTICITY;
            g->x = nx;
        }
    }
}

static void step_grain(sim_t *s, sim_grain_t *g, int cx, int cy)
{
    int supported = 0;
    if (s->down >= 0) {
        supported = !is_free(s, cx + DIR_X[s->down], cy + DIR_Y[s->down]);
    }

    float speed2 = sim_grain_speed2(g);
    if (supported && speed2 < REST_SPEED * REST_SPEED) {
        // Resting on the pile: static friction holds it unless the slope is too steep.
        g->vx = 0.0f;
        g->vy = 0.0f;
        if (rng_unit(s) < s->repose_prob && try_repose(s, g, cx, cy)) {
            s->motion_acc += 0.8f * REST_SPEED; // the token slide velocity
        }
        return;
    }

    // Integrate gravity, drag and jitter.
    g->vx = (g->vx + s->ax) * (1.0f - DAMPING);
    g->vy = (g->vy + s->ay) * (1.0f - DAMPING);
    if (speed2 > REST_SPEED * REST_SPEED) {
        g->vx += JITTER * rng_signed(s);
        g->vy += JITTER * rng_signed(s);
    }

    // Sliding friction against the pile below.
    if (supported) {
        float dxn = (float)DIR_X[s->down], dyn = (float)DIR_Y[s->down];
        float inv = (DIR_X[s->down] && DIR_Y[s->down]) ? 0.70710678f : 1.0f;
        dxn *= inv;
        dyn *= inv;
        float vn = g->vx * dxn + g->vy * dyn;
        float tx = g->vx - vn * dxn, ty = g->vy - vn * dyn;
        g->vx -= CONTACT_FRICTION * tx;
        g->vy -= CONTACT_FRICTION * ty;
    }

    // Coulomb friction against the screen face: a fixed deceleration that
    // also holds still grains in place when the pull is weaker than it.
    float sp2 = sim_grain_speed2(g);
    if (sp2 <= s->friction * s->friction) {
        g->vx = 0.0f;
        g->vy = 0.0f;
        return;
    }
    float inv_sp = rsqrt(sp2);
    float sp = sp2 * inv_sp;
    float k = 1.0f - s->friction * inv_sp;
    if (sp * k > MAX_SPEED) {
        k = MAX_SPEED * inv_sp;
    }
    g->vx *= k;
    g->vy *= k;
    s->motion_acc += sp * k;

    move_grain(s, g, cx, cy);
}

// Sleeping grains are skipped. A grain falls asleep after SLEEP_STEPS steps
// without moving and is woken by a neighbour leaving, a gravity change, a
// shake or a respawn.
static inline void update_grain(sim_t *s, int i)
{
    if (s->idle[i] >= SLEEP_STEPS) {
        return;
    }
    s->awake++;
    sim_grain_t *g = &s->grains[i];
    const int cx = (int)g->x;
    const int cy = (int)g->y;

    step_grain(s, g, cx, cy);

    if ((int)g->x != cx || (int)g->y != cy) {
        s->idle[i] = 0;
        wake_around(s, cx, cy);
    } else if (g->vx == 0.0f && g->vy == 0.0f) {
        s->idle[i]++; // < SLEEP_STEPS here, so no overflow
        s->sleeping += s->idle[i] == SLEEP_STEPS;
    } else {
        s->idle[i] = 0;
    }
}

void sim_step(sim_t *s)
{
    const int n = s->n;
    if (n == 0) {
        return;
    }
    // Alternate direction and start at a random grain so no part of the pile
    // is systematically updated first.
    int start = (int)(rng_next(s) % (uint32_t)n);
    int reverse = (int)(s->steps & 1);

    s->motion_acc = 0.0f;
    s->awake = 0;
    int idx = start;
    for (int c = 0; c < n; c++) {
        update_grain(s, idx);
        if (reverse) {
            idx = (idx == 0) ? n - 1 : idx - 1;
        } else {
            idx = (idx + 1 == n) ? 0 : idx + 1;
        }
    }

    s->motion = s->motion_acc;
    s->steps++;
}
