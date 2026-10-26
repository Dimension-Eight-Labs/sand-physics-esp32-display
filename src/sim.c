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
#include "material.h"

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

// floor() for the coordinates used here: x > -1.0 always (positions are >= 0
// and a sub-step moves at most one cell), so only (-1, 0) needs fixing up.
static inline int ifloor(float x)
{
    return x < 0.0f ? -1 : (int)x;
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

// Materials on the coherent motion path (step_liquid / move_liquid, update
// order along gravity, grounding, surface-only waking): liquid and sand.
static inline int coherent(const material_t *m)
{
    return m->rest_rule == MAT_REST_SPREAD || m->flow_model == MAT_FLOW_COHERENT;
}

// 1/n for the sub-step count (n <= SIM_MAX_SUBSTEPS).
#define SIM_MAX_SUBSTEPS 8
static const float INV_N[SIM_MAX_SUBSTEPS + 1] = {
    1.0f, 1.0f, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8,
};

// ---- Grid helpers -------------------------------------------------------------

static inline int in_bounds(const sim_t *s, int x, int y)
{
    return (unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h;
}

// Free = inside the grid and not occupied. Walls count as blocked.
static inline int is_free(const sim_t *s, int x, int y)
{
    return in_bounds(s, x, y) && s->cell[y * s->w + x] == 0;
}

static inline void wake_all(sim_t *s)
{
    s->n_wake_all++;
    s->input_step = s->steps;
    memset(s->idle, 0, (size_t)s->n);
    s->sleeping = 0;
}

// A grain left (x, y): wake sleepers around it, since the hole may let them
// fall or slide. Further-away effects propagate as those grains move.
static HOT_FN void wake_around(sim_t *s, int x, int y)
{
    if (s->sleeping == 0) {
        return; // common while everything is in motion: nothing to wake
    }
    const int r = s->wake_radius;
    for (int dy = -r; dy <= r; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= s->h) {
            continue;
        }
        const uint16_t *row = &s->cell[yy * s->w];
        for (int dx = -r; dx <= r; dx++) {
            int xx = x + dx;
            if (xx >= 0 && xx < s->w && row[xx]) {
                uint8_t *idle = &s->idle[row[xx] - 1];
                s->sleeping -= *idle >= SLEEP_STEPS;
                *idle = 0;
            }
        }
    }
}

// Liquid only: a gravity change can only move grains that have somewhere to
// go, so wake just the sleepers with a free cell beside, above or below
// them. Fully surrounded grains (most of a pool) wake later through
// wake_around() if a neighbour leaves; so does the rare grain whose only
// free neighbour is diagonal. (Sand keeps wake_all() so CLASSIC stays
// bit-identical to v1.)
static HOT_FN void wake_exposed(sim_t *s)
{
    s->n_wake_exposed++;
    s->input_step = s->steps;
    if (s->sleeping == 0) {
        return;
    }
    const int w = s->w, h = s->h;
    const uint16_t *cell = s->cell;
    for (int i = 0; i < s->n; i++) {
        if (s->idle[i] < SLEEP_STEPS) {
            continue;
        }
        const int x = (int)s->grains[i].x, y = (int)s->grains[i].y;
        const int idx = y * w + x;
        if ((x > 0 && !cell[idx - 1]) || (x < w - 1 && !cell[idx + 1]) || (y > 0 && !cell[idx - w]) ||
            (y < h - 1 && !cell[idx + w])) {
            s->idle[i] = 0;
            s->sleeping--;
        }
    }
}

static inline void cell_move(sim_t *s, int from_x, int from_y, int to_x, int to_y)
{
    uint16_t id = s->cell[from_y * s->w + from_x];
    s->cell[from_y * s->w + from_x] = 0;
    s->cell[to_y * s->w + to_x] = id;
    s->moves++;
}

// ---- Setup --------------------------------------------------------------------

size_t sim_mem_bytes(int w, int h, int n)
{
    size_t grains = (size_t)n * sizeof(sim_grain_t);
    size_t cells = (size_t)w * h * sizeof(uint16_t);
    size_t bytes = ((size_t)n + 3) & ~(size_t)3; // shade, then idle
    size_t cols = (size_t)(w + h) * sizeof(sim_column_t); // liquid levelling
    size_t waves = (size_t)(w + h) * (3 * sizeof(float) + 2 * sizeof(int16_t)) + sizeof(float) + 4; // flux, acc, prefix, order, counts
    size_t order = 4 + (size_t)n * sizeof(uint16_t) + (size_t)2 * (w + h) * sizeof(uint16_t); // + alignment
    return grains + cells + 3 * bytes + cols + waves + order; // + ground flags
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
    p += ((size_t)n + 3) & ~(size_t)3;
    s->cols = (sim_column_t *)p;
    p += (size_t)(w + h) * sizeof(sim_column_t);
    s->ground = p;
    memset(s->ground, 0, (size_t)n);
    p += ((size_t)n + 3) & ~(size_t)3;
    s->flux = (float *)p;
    memset(s->flux, 0, (size_t)(w + h) * sizeof(float));
    p += (size_t)(w + h) * sizeof(float);
    s->wave_acc = (float *)p;
    memset(s->wave_acc, 0, (size_t)(w + h) * sizeof(float));
    p += (size_t)(w + h) * sizeof(float);
    s->wave_pre = (float *)p;
    p += (size_t)(w + h + 1) * sizeof(float);
    s->col_order = (int16_t *)p;
    p += (size_t)(w + h) * sizeof(int16_t);
    s->wave_cnt = (int16_t *)p;
    p += (size_t)(w + h + 1) * sizeof(int16_t);
    p = (uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
    s->order = (uint16_t *)p;
    p += (size_t)n * sizeof(uint16_t);
    s->order_cnt = (uint16_t *)p;
    s->flux_down = -1;
    s->wave_active = 0;

    s->rng = seed ? seed : 0x9E3779B9u;
    s->down = -1;
    s->wake_cos = cosf(WAKE_ANGLE_DEG * DEG2RAD);
    s->level_recheck_cos = cosf(LEVEL_RECHECK_DEG * DEG2RAD);
    sim_set_material(s, material_get(MATERIAL_CLASSIC));
    memset(s->cell, 0, (size_t)w * h * sizeof(uint16_t));
}

void sim_spawn(sim_t *s, float region, int n_shades)
{
    memset(s->cell, 0, (size_t)s->w * s->h * sizeof(uint16_t));
    wake_all(s);
    s->level_settled = 0;
    memset(s->ground, 0, (size_t)s->n);
    s->flux_down = -1;
    s->wave_active = 0;

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
    const material_t *m = s->mat;
    const float g_xy = sqrtf(gx * gx + gy * gy);
    const float glass = m->glass_friction * fabsf(gz); // friction, in g
    s->ax = gx * m->gravity_scale;
    s->ay = gy * m->gravity_scale;
    s->friction = glass * m->gravity_scale;

    // The pile only "feels" a down direction when the in-plane pull beats the
    // static friction against the glass; otherwise it is frozen in place.
    const float net = g_xy - glass;
    if (net < GRAVITY_DEADZONE) {
        s->down = -1;
        s->rule_prob = 0.0f;
        return;
    }

    // Snap to the nearest of 8 directions, remembering the residual tilt.
    float ang = atan2f(gy, gx) / (45.0f * DEG2RAD); // in units of 45°
    float k = floorf(ang + 0.5f);
    float tilt_deg = (ang - k) * 45.0f;             // -22.5 .. 22.5, positive toward k+1
    s->down = ((int)k % 8 + 8) % 8;

    // Resting grains act less often under a weaker net pull (fall time ~ 1/sqrt(g)).
    s->rule_prob = m->rule_prob * fminf(1.0f, sqrtf(net));

    if (m->rest_rule == MAT_REST_SPREAD) {
        // Liquid levels perpendicular to true gravity, which leans by tilt_deg
        // from the snapped frame: moving k cells toward the side it leans to
        // lowers a grain by k * tan(tilt).
        float t = tanf(tilt_deg * DEG2RAD);
        s->tilt_tan[1] = t;
        s->tilt_tan[0] = -t;
        s->up_x = -gx / g_xy;
        s->up_y = -gy / g_xy;
        return;
    }

    s->up_x = -gx / g_xy; // (sand's landing test; gravel does not use it)
    s->up_y = -gy / g_xy;

    // Steepest stable slope. A grain on a slope θ is held by internal friction
    // (tan(repose) · g_xy·cosθ) plus glass friction (BACK_FRICTION · |gz|):
    //   tan θ_max = tan(repose) + glass / (g_xy · cos θ)
    // so piles stand steeper the further the board leans back.
    // Diagonal frames under-measure drops by about a cell, hence the correction.
    float base = m->repose_angle_deg - ((s->down & 1) ? m->repose_diag_correction_deg : 0.0f);
    float tan_base = tanf(base * DEG2RAD) + glass / (g_xy * cosf(base * DEG2RAD));
    base = atanf(tan_base) / DEG2RAD;

    // Lateral side +1 is direction down+2 (90° toward k+1), side -1 is down-2.
    // Gravity leaning toward a side makes that side's slope effectively steeper.
    float thr_pos = fminf(base - tilt_deg, 89.0f);
    float thr_neg = fminf(base + tilt_deg, 89.0f);
    s->repose_tan[1] = tanf(thr_pos * DEG2RAD);
    s->repose_tan[0] = tanf(thr_neg * DEG2RAD);
}

// Liquid: recompute every grounded flag at once for the current "down". In
// each line along gravity, the run of grains packed against the wall at its
// far end is grounded; anything above a gap is not (it will fall). Used when
// "down" changes, so a resting pool stays at rest across a frame change while
// a flipped one falls as a whole.
static void ground_sweep(sim_t *s)
{
    for (int i = 0; i < s->n; i++) {
        s->ground[i] &= LIQ_SPRAY;
    }
    if (s->down < 0) {
        return;
    }
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    // Start at the far end: edge cells whose successor along gravity is outside.
    for (int e = 0; e < 2 * s->w + 2 * (s->h - 2); e++) {
        int x, y;
        if (e < s->w) {
            x = e, y = 0;
        } else if (e < 2 * s->w) {
            x = e - s->w, y = s->h - 1;
        } else if (e < 2 * s->w + s->h - 2) {
            x = 0, y = e - 2 * s->w + 1;
        } else {
            x = s->w - 1, y = e - (2 * s->w + s->h - 2) + 1;
        }
        if (in_bounds(s, x + dx, y + dy)) {
            continue;
        }
        while (in_bounds(s, x, y) && s->cell[y * s->w + x]) {
            s->ground[s->cell[y * s->w + x] - 1] |= LIQ_GROUND;
            x -= dx;
            y -= dy;
        }
    }
}

void sim_set_gravity(sim_t *s, float gx, float gy, float gz)
{
    s->last_g[0] = gx;
    s->last_g[1] = gy;
    s->last_g[2] = gz;
    apply_gravity(s, gx, gy, gz);

    // Sleeping grains were stable under the gravity they fell asleep with
    // (ref_g, recorded at the last wake). Wake everyone once gravity has moved
    // far enough from that: another snapped direction or the pile
    // (un)freezing, the in-plane direction turning by WAKE_ANGLE_DEG, or the
    // in-plane / normal magnitude changing by WAKE_PULL_DELTA_G. Comparing
    // against the reference, not the previous frame, means slow drift
    // still wakes the pile while tremor around one attitude does not.
    int wake = s->down != s->ref_down;
    if (!wake && s->down >= 0) { // frozen and still frozen: nothing can move
        const float *r = s->ref_g;
        float g_xy = sqrtf(gx * gx + gy * gy);
        float r_xy = sqrtf(r[0] * r[0] + r[1] * r[1]);
        wake = gx * r[0] + gy * r[1] < s->wake_cos * g_xy * r_xy ||
               fabsf(g_xy - r_xy) > WAKE_PULL_DELTA_G ||
               fabsf(fabsf(gz) - fabsf(r[2])) > WAKE_PULL_DELTA_G;
    }
    if (wake) {
        if (coherent(s->mat)) {
            if (s->down != s->ref_down) {
                // "Down" changed (a flip or a big tilt): wake the whole body and
                // ground what rests against the new floor; the rest falls.
                wake_all(s);
                ground_sweep(s);
            } else {
                wake_exposed(s); // small change, e.g. hand sway: only the surface
            }
        } else {
            wake_all(s);
        }
        s->ref_g[0] = gx;
        s->ref_g[1] = gy;
        s->ref_g[2] = gz;
        s->ref_down = s->down;
    }
}

void sim_set_material(sim_t *s, const material_t *m)
{
    s->mat = m;
    s->rest_speed2 = m->rest_speed * m->rest_speed;
    s->damp_keep = 1.0f - m->damping;
    s->token = m->rule_token * m->rest_speed;
    s->wake_radius = m->wake_radius ? m->wake_radius : WAKE_RADIUS;
    s->level_settled = 0;
    // New rules for every grain: re-derive the gravity terms and wake all.
    apply_gravity(s, s->last_g[0], s->last_g[1], s->last_g[2]);
    wake_all(s);
    ground_sweep(s); // what rests against the floor is grounded; the rest falls
    s->ref_g[0] = s->last_g[0];
    s->ref_g[1] = s->last_g[1];
    s->ref_g[2] = s->last_g[2];
    s->ref_down = s->down;
}

// Liquid knock, the only liquid splash. The top knock_depth grains of each
// column (surface grains resting on something) are candidates; at most
// knock_max of them per knock are thrown, picked at random. Each flies off in
// its own direction within +/- knock_fan_deg of the splash direction (up
// against gravity, leaning the way of the knock), never within
// LIQUID_KNOCK_MIN_DEG of straight up its column, at a random knock_splash-scaled speed, so a knock throws a loose
// fan of droplets rather than lines. Thrown grains are spray. The rest of the
// body gets a knock_interior share of the push along the knock (sloshing).
// Impulses closer than LIQUID_KNOCK_GAP steps are one knock and share the
// cap; under sustained shaking the cap renews every LIQUID_KNOCK_RENEW steps.
#define LIQUID_KNOCK_GAP 6
#define LIQUID_KNOCK_RENEW 30
#define LIQUID_KNOCK_MIN_DEG 8.0f

static int knock_candidate(const sim_t *s, int i, int ux, int uy, int depth)
{
    if (s->ground[i] & LIQ_SPRAY) {
        return 0; // already flying
    }
    const sim_grain_t *g = &s->grains[i];
    const int x = (int)g->x, y = (int)g->y;
    if (in_bounds(s, x - ux, y - uy) && !s->cell[(y - uy) * s->w + (x - ux)]) {
        return 0; // nothing under it: in the air
    }
    for (int d = 1; d <= depth; d++) {
        const int ax = x + d * ux, ay = y + d * uy;
        if (!in_bounds(s, ax, ay)) {
            return 0; // column full to the wall
        }
        if (!s->cell[ay * s->w + ax]) {
            return 1; // free cell within depth above it
        }
    }
    return 0;
}

static void liquid_knock(sim_t *s, float dir_x, float dir_y, float strength)
{
    const material_t *m = s->mat;
    if (strength <= 0.0f) {
        return;
    }
    // One knock (diagnostics) and its grain budget.
    const int fresh = s->n_knocks == 0 || s->steps - s->knock_step > LIQUID_KNOCK_GAP;
    if (fresh || s->steps - s->knock_start > LIQUID_KNOCK_RENEW) {
        s->knock_left = m->knock_max;
        s->knock_start = s->steps;
    }
    s->n_knocks += fresh;
    s->knock_step = s->steps;

    // "Up" against gravity: in cells for the column test, as a unit vector
    // for the splash. Lying flat (no in-plane gravity) uses screen up.
    float upx = s->up_x, upy = s->up_y;
    int ux, uy;
    if (s->down >= 0) {
        ux = -DIR_X[s->down];
        uy = -DIR_Y[s->down];
    } else {
        ux = 0;
        uy = -1;
    }
    if (upx * upx + upy * upy < 0.25f) {
        upx = (float)ux;
        upy = (float)uy;
    }
    // Splash direction: up, leaning about 20 deg toward the knock; as an
    // angle from up (+ = clockwise on screen).
    const float bx = upx + 0.35f * dir_x, by = upy + 0.35f * dir_y;
    const float base = atan2f(upx * by - upy * bx, upx * bx + upy * by);
    const int depth = m->knock_depth < 1 ? 1 : m->knock_depth;

    int cand = 0;
    for (int i = 0; i < s->n; i++) {
        cand += knock_candidate(s, i, ux, uy, depth);
    }
    const float p = cand > 0 ? (float)s->knock_left / (float)cand : 0.0f;
    const float fan = m->knock_fan_deg * DEG2RAD;
    const float push = strength * m->knock_splash * m->knock_interior;
    for (int i = 0; i < s->n; i++) {
        sim_grain_t *g = &s->grains[i];
        if (s->knock_left > 0 && knock_candidate(s, i, ux, uy, depth) && rng_unit(s) < p) {
            // Anywhere in the fan, but at least LIQUID_KNOCK_MIN_DEG off
            // straight up its own column.
            float a = base + fan * rng_signed(s);
            if (fabsf(a) < LIQUID_KNOCK_MIN_DEG * DEG2RAD) {
                a = (a < 0.0f ? -LIQUID_KNOCK_MIN_DEG : LIQUID_KNOCK_MIN_DEG) * DEG2RAD;
            }
            const float c = cosf(a), sn = sinf(a);
            const float v = strength * m->knock_splash * (0.6f + 0.4f * rng_unit(s));
            g->vx = (upx * c - upy * sn) * v;
            g->vy = (upx * sn + upy * c) * v;
            s->ground[i] |= LIQ_SPRAY;
            s->knock_left--;
            s->n_knock_grains++;
        } else if (push > 0.0f) {
            const float d = push * (0.5f + 0.5f * rng_unit(s));
            g->vx += dir_x * d;
            g->vy += dir_y * d;
        }
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
    if (s->mat->rest_rule == MAT_REST_SPREAD && s->mat->knock_splash > 0.0f) {
        liquid_knock(s, dir_x, dir_y, strength);
        return;
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
static HOT_FN int try_repose(sim_t *s, sim_grain_t *g, int cx, int cy)
{
    const material_t *m = s->mat;
    const int scan = m->repose_scan;
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    int first = (int)(rng_next(s) & 1);

    for (int t = 0; t < 2; t++) {
        int side = first ^ t;                        // 0 => side -1, 1 => side +1
        int lat = (s->down + (side ? 2 : 6)) & 7;
        const int sx = DIR_X[lat], sy = DIR_Y[lat];
        // Dithering the threshold lets the average slope fall between the
        // few slopes a small grid can represent (2/3, 3/4, 1, ...).
        const float tan_thr = s->repose_tan[side] * (1.0f + m->repose_dither * rng_signed(s));

        int slide = 0;
        for (int k = 1; k <= scan && !slide; k++) {
            int px = cx + k * sx, py = cy + k * sy;
            if (!is_free(s, px, py)) {
                break; // surface rises or wall: nothing further out matters
            }
            int drop = 0;
            while (drop < scan && is_free(s, px + (drop + 1) * dx, py + (drop + 1) * dy)) {
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
        // rest_speed, so a supported grain is back at rest next step.
        g->vx = (float)(tx - cx) * s->token * 0.7071f;
        g->vy = (float)(ty - cy) * s->token * 0.7071f;
        return 1;
    }
    return 0;
}

// Spread rule (liquid) for a resting grain at (cx, cy): look up to spread_scan
// cells along each lateral direction (random side first) for a spot where the
// grain would end up at least one cell lower in true gravity, either by
// dropping into a free cell below that spot or because gravity leans toward
// that side. If found, push the grain that way at spread_push cells/step; it
// then slides with momentum (liquid has almost no contact friction), which
// is what makes it slosh, and rests again once nothing lower is in reach. On
// a level surface nothing qualifies, so the grain rests and soon sleeps.
// Returns 1 if it was pushed.
static HOT_FN int try_spread(sim_t *s, sim_grain_t *g, int cx, int cy)
{
    const int scan = s->mat->spread_scan;
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    int first = (int)(rng_next(s) & 1);

    for (int t = 0; t < 2; t++) {
        int side = first ^ t;
        int lat = (s->down + (side ? 2 : 6)) & 7;
        const int sx = DIR_X[lat], sy = DIR_Y[lat];
        const float lean = s->tilt_tan[side]; // height lost per lateral cell

        int found = 0;
        for (int k = 1; k <= scan && !found; k++) {
            int px = cx + k * sx, py = cy + k * sy;
            if (!is_free(s, px, py)) {
                break;
            }
            // Drop needed below this spot to end >= 1 cell lower overall.
            float need = 1.0f - (float)k * lean;
            int drop = 0;
            while ((float)drop < need && is_free(s, px + (drop + 1) * dx, py + (drop + 1) * dy)) {
                drop++;
            }
            if ((float)drop >= need) {
                found = k;
            }
        }
        if (!found) {
            continue;
        }
        // Flow: jump along the free path toward the spot (at most spread_step
        // cells this step; every cell up to `found` is known to be free).
        const int step = s->mat->spread_step;
        int j = found < step ? found : step;
        if (j > 0) {
            // Then settle down onto the surface below, at the same rate, so
            // the grain flows instead of hopping and falling from rest.
            int tx = cx + j * sx, ty = cy + j * sy;
            int d = 0;
            while (d < step && is_free(s, tx + dx, ty + dy)) {
                tx += dx;
                ty += dy;
                d++;
            }
            cell_move(s, cx, cy, tx, ty);
            g->x += (float)(tx - cx);
            g->y += (float)(ty - cy);
        }
        const float push = (sx && sy) ? s->mat->spread_push * 0.70710678f : s->mat->spread_push;
        g->vx = (float)sx * push;
        g->vy = (float)sy * push;
        s->n_pushes++;
        return 1;
    }
    return 0;
}

// Move a grain along its velocity in sub-steps of at most one cell, resolving
// collisions axis by axis.
static HOT_FN void move_grain(sim_t *s, sim_grain_t *g, int cx, int cy)
{
    const float elasticity = s->mat->elasticity;
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
                    g->vy = -g->vy * elasticity;
                    done = 1;
                } else if (!try_x && is_free(s, cx, ncy)) {
                    cell_move(s, cx, cy, cx, ncy);
                    cy = ncy;
                    g->y = ny;
                    g->vx = -g->vx * elasticity;
                    done = 1;
                }
            }
            if (!done) {
                g->vx = -g->vx * elasticity;
                g->vy = -g->vy * elasticity;
                return;
            }
        } else if (move_x) {
            g->vx = -g->vx * elasticity; // x blocked, y stays inside this cell
            g->y = ny;
        } else {
            g->vy = -g->vy * elasticity;
            g->x = nx;
        }
    }
}

static HOT_FN void step_grain(sim_t *s, sim_grain_t *g, int cx, int cy)
{
    const material_t *m = s->mat;
    int supported = 0;
    if (s->down >= 0) {
        supported = !is_free(s, cx + DIR_X[s->down], cy + DIR_Y[s->down]);
    }

    float speed2 = sim_grain_speed2(g);
    if (supported && speed2 < s->rest_speed2) {
        // Resting on the pile: static friction holds it unless its resting
        // rule (slope too steep / somewhere lower to flow to) moves it.
        g->vx = 0.0f;
        g->vy = 0.0f;
        if (m->rest_rule == MAT_REST_SPREAD) {
            // Liquid with both sideways cells taken cannot spread: skip the
            // look-ahead (and its random draw) for the whole submerged body.
            const int l1 = (s->down + 2) & 7, l2 = (s->down + 6) & 7;
            if (!is_free(s, cx + DIR_X[l1], cy + DIR_Y[l1]) && !is_free(s, cx + DIR_X[l2], cy + DIR_Y[l2])) {
                return;
            }
        }
        if (rng_unit(s) < s->rule_prob &&
            (m->rest_rule == MAT_REST_SPREAD ? try_spread(s, g, cx, cy) : try_repose(s, g, cx, cy))) {
            s->motion_acc += m->rest_rule == MAT_REST_SPREAD ? m->spread_push : s->token;
        }
        return;
    }

    // Integrate gravity, drag and jitter.
    g->vx = (g->vx + s->ax) * s->damp_keep;
    g->vy = (g->vy + s->ay) * s->damp_keep;
    if (speed2 > s->rest_speed2) {
        g->vx += m->jitter * rng_signed(s);
        g->vy += m->jitter * rng_signed(s);
    }

    // Sliding friction against the pile below.
    if (supported) {
        float dxn = (float)DIR_X[s->down], dyn = (float)DIR_Y[s->down];
        float inv = (DIR_X[s->down] && DIR_Y[s->down]) ? 0.70710678f : 1.0f;
        dxn *= inv;
        dyn *= inv;
        float vn = g->vx * dxn + g->vy * dyn;
        float tx = g->vx - vn * dxn, ty = g->vy - vn * dyn;
        g->vx -= m->contact_friction * tx;
        g->vy -= m->contact_friction * ty;
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
    if (sp * k > m->max_speed) {
        k = m->max_speed * inv_sp;
    }
    g->vx *= k;
    g->vy *= k;
    s->motion_acc += sp * k;

    move_grain(s, g, cx, cy);
}

// ---- Liquid (and sand) step -------------------------------------------------------------
//
// Liquid uses the same integration as gravel but its own support test and
// collision response (gravel's code above is untouched, so CLASSIC stays
// bit-identical to v1). Sand (MAT_FLOW_COHERENT) moves the same way, with the
// liquid-only parts set to zero in its parameters (no viscosity, absorption,
// flow, knock or levelling) and gravel's angle-of-repose rule at rest:
//  - Supported means resting on a wall or on a grain that is not falling away;
//    a grain on top of a falling grain is falling too. The spread rule only
//    acts on grains grounded through a still chain down to a wall, so nothing
//    is pushed sideways while the body above is still dropping.
//  - A grain blocked by a grain moving the same way keeps up with it instead
//    of bouncing off it, so a falling body falls as a body.
//  - Moving grains share momentum with their neighbours (viscosity): the body
//    moves, falls and lands as sheets and blobs instead of loose grains.
//  - Landing never splashes: the landing grain's velocity is pulled toward
//    the mean of the liquid around it (impact_absorb), so a pour joins the
//    local flow calmly. impact_to_flow (0 by default) can add some sideways
//    flow, never faster than the landing.
//  - The only splash is a knock (liquid_knock). Thrown grains are spray: when
//    spray lands it is absorbed with no bounce or flow; the mark clears once
//    the grain rests.

#define LIQUID_FALL_AWAY 0.05f  // cells/step: a supporter moving away faster is falling
#define LIQUID_SPRAY_KEEP 0.5f  // sideways velocity kept by spray landing on the body

// Mean velocity of the grains in the 4 cells around (cx, cy); returns how
// many there are (0: *mx, *my untouched).
static inline int liquid_nb_mean(const sim_t *s, int cx, int cy, float *mx, float *my)
{
    const uint16_t *cell = s->cell;
    const int w = s->w, idx = cy * w + cx;
    const int nb[4] = { cx > 0 ? cell[idx - 1] : 0, cx < w - 1 ? cell[idx + 1] : 0, cy > 0 ? cell[idx - w] : 0,
                        cy < s->h - 1 ? cell[idx + w] : 0 };
    float sx = 0.0f, sy = 0.0f;
    int c = 0;
    for (int k = 0; k < 4; k++) {
        if (nb[k]) {
            sx += s->grains[nb[k] - 1].vx;
            sy += s->grains[nb[k] - 1].vy;
            c++;
        }
    }
    if (c) {
        *mx = sx * INV_N[c];
        *my = sy * INV_N[c];
    }
    return c;
}

// Grain g (index gi) at (cx, cy) is blocked on one axis by the cell (bx, by).
// Resolve the velocity component on that axis.
// Returns 1 if the grain is waiting behind a moving / falling grain (the
// same cell will block it again this step), 0 after an impact.
static HOT_FN int liquid_hit(sim_t *s, sim_grain_t *g, int gi, int axis_x, int bx, int by, int cx, int cy)
{
    const material_t *m = s->mat;
    float *vn = axis_x ? &g->vx : &g->vy;
    float *vt = axis_x ? &g->vy : &g->vx;

    // Following a grain that moves the same way: keep up with it, no bounce.
    // A grain that is not grounded is part of a falling body even if it has
    // not moved yet this step: wait behind it with the velocity kept.
    // Impacts (sideways flow, splashes) happen only on walls and grounded grains.
    if (in_bounds(s, bx, by)) {
        int id = s->cell[by * s->w + bx];
        if (id) {
            const sim_grain_t *b = &s->grains[id - 1];
            float bv = axis_x ? b->vx : b->vy;
            if (bv * *vn > 0.0f) {
                if (fabsf(bv) < fabsf(*vn)) {
                    *vn = bv;
                }
                return 1;
            }
            if (!(s->ground[id - 1] & LIQ_GROUND)) {
                // At most one step of gravity faster than the grain ahead, so
                // waiting never stores up a fake impact.
                const float lim = fabsf(bv) + s->mat->gravity_scale;
                if (fabsf(*vn) > lim) {
                    *vn = *vn > 0.0f ? lim : -lim;
                }
                return 1;
            }
        }
    }

    // Impact.
    const float sp = fabsf(*vn);
    const int dir = *vn > 0.0f ? 1 : -1;
    // Moving with gravity on this axis, faster than resting: a landing.
    const float gdot = (float)dir * (axis_x ? -s->up_x : -s->up_y);
    const int landing = gdot > 0.3f && sp > s->mat->rest_speed;

    if (s->ground[gi] & LIQ_SPRAY) {
        // Spray is absorbed into the body: no bounce, no flow.
        *vn = 0.0f;
        *vt *= LIQUID_SPRAY_KEEP;
        s->n_spray_landings += landing;
    } else {
        // Optional sideways flow from the landing (impact_to_flow, 0 by
        // default). Only real landings feed it, not the steady press of
        // gravity on a grain lying on the surface, and it is never faster
        // than the landing (or the flow the grain already had).
        if (sp > s->mat->rest_speed && m->impact_to_flow > 0.0f) {
            const int ox = axis_x ? 0 : 1, oy = axis_x ? 1 : 0; // the other axis
            int side;
            if (fabsf(*vt) > 0.05f) {
                side = *vt > 0.0f ? 1 : -1; // keep flowing the way it was going
            } else {
                int fp = is_free(s, cx + ox, cy + oy), fm = is_free(s, cx - ox, cy - oy);
                side = fp && fm ? ((rng_next(s) & 1) ? 1 : -1) : fp ? 1 : fm ? -1 : 0;
            }
            const float vt0 = fabsf(*vt);
            const float lim = vt0 > sp * m->impact_to_flow ? vt0 : sp * m->impact_to_flow;
            float t = *vt + (float)side * sp * m->impact_to_flow;
            *vt = t > lim ? lim : t < -lim ? -lim : t;
        }
        *vn = -*vn * m->elasticity;
    }

    // A landing grain joins the local flow: its velocity is pulled toward the
    // mean of the liquid around it, so the impact is damped away instead of
    // bouncing or splashing.
    if (landing && m->impact_absorb > 0.0f) {
        float mx, my;
        if (liquid_nb_mean(s, cx, cy, &mx, &my)) {
            g->vx += (mx - g->vx) * m->impact_absorb;
            g->vy += (my - g->vy) * m->impact_absorb;
            s->n_landings++;
        }
    }
    return 0;
}

// move_grain() with liquid's collision response.
static HOT_FN void move_liquid(sim_t *s, sim_grain_t *g, int gi, int cx, int cy)
{
    float ax = fabsf(g->vx), ay = fabsf(g->vy);
    float m = ax > ay ? ax : ay;
    if (m <= 0.0f) {
        return;
    }
    int n = (int)m;
    n += (float)n < m;
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
            int x_first = fabsf(g->vx) >= fabsf(g->vy);
            int done = 0;
            for (int a = 0; a < 2 && !done; a++) {
                int try_x = (a == 0) == x_first;
                if (try_x && is_free(s, ncx, cy)) {
                    cell_move(s, cx, cy, ncx, cy);
                    cx = ncx;
                    g->x = nx;
                    if (liquid_hit(s, g, gi, 0, cx, ncy, cx, cy)) {
                        return; // waiting: the same grain blocks the rest of this step
                    }
                    done = 1;
                } else if (!try_x && is_free(s, cx, ncy)) {
                    cell_move(s, cx, cy, cx, ncy);
                    cy = ncy;
                    g->y = ny;
                    if (liquid_hit(s, g, gi, 1, ncx, cy, cx, cy)) {
                        return;
                    }
                    done = 1;
                }
            }
            if (!done) {
                liquid_hit(s, g, gi, 1, ncx, cy, cx, cy);
                liquid_hit(s, g, gi, 0, cx, ncy, cx, cy);
                return;
            }
        } else if (move_x) {
            int wait = liquid_hit(s, g, gi, 1, ncx, cy, cx, cy);
            g->y = ny;
            if (wait) {
                return;
            }
        } else {
            int wait = liquid_hit(s, g, gi, 0, cx, ncy, cx, cy);
            g->x = nx;
            if (wait) {
                return;
            }
        }
    }
}

static HOT_FN void step_liquid(sim_t *s, sim_grain_t *g, int i, int cx, int cy)
{
    const material_t *m = s->mat;
    int supported = 0, grounded = 0;
    if (s->down >= 0) {
        const int bx = cx + DIR_X[s->down], by = cy + DIR_Y[s->down];
        if (!in_bounds(s, bx, by)) {
            supported = grounded = 1; // wall
        } else {
            const int id = s->cell[by * s->w + bx];
            if (id) {
                const sim_grain_t *b = &s->grains[id - 1];
                float away = b->vx * (float)DIR_X[s->down] + b->vy * (float)DIR_Y[s->down];
                supported = away < LIQUID_FALL_AWAY;
                // A sleeping supporter has not moved for SLEEP_STEPS steps: solid.
                grounded = supported && ((s->ground[id - 1] & LIQ_GROUND) || s->idle[id - 1] >= SLEEP_STEPS);
            }
        }
    }
    s->ground[i] = (uint8_t)((s->ground[i] & LIQ_SPRAY) | (grounded ? LIQ_GROUND : 0u));

    // Only grains grounded through a still chain down to a wall can rest.
    // After a flip nothing is grounded, so the whole body falls at once
    // (followers keep pace with the grain ahead, see liquid_hit) instead of
    // peeling off its leading edge one row per step.
    float speed2 = sim_grain_speed2(g);
    if (grounded && speed2 < s->rest_speed2) {
        g->vx = 0.0f;
        g->vy = 0.0f;
        s->ground[i] = LIQ_GROUND; // at rest: no longer spray
        // Both sideways cells taken: cannot spread or slide (the whole
        // submerged body); skip the look-ahead and its random draws.
        const int l1 = (s->down + 2) & 7, l2 = (s->down + 6) & 7;
        if (!is_free(s, cx + DIR_X[l1], cy + DIR_Y[l1]) && !is_free(s, cx + DIR_X[l2], cy + DIR_Y[l2])) {
            return;
        }
        if (m->rest_rule == MAT_REST_REPOSE) {
            // Sand: the angle-of-repose rule, as gravel.
            if (rng_unit(s) < s->rule_prob && try_repose(s, g, cx, cy)) {
                s->motion_acc += s->token;
            }
            return;
        }
        if (rng_unit(s) < s->rule_prob && try_spread(s, g, cx, cy)) {
            s->motion_acc += m->spread_push;
        }
        return;
    }

    // Viscosity: pull toward the mean velocity of the grains around it.
    if (m->viscosity > 0.0f) {
        float mx, my;
        if (liquid_nb_mean(s, cx, cy, &mx, &my)) {
            g->vx += (mx - g->vx) * m->viscosity;
            g->vy += (my - g->vy) * m->viscosity;
        }
    }

    g->vx = (g->vx + s->ax) * s->damp_keep;
    g->vy = (g->vy + s->ay) * s->damp_keep;
    if (speed2 > s->rest_speed2) {
        g->vx += m->jitter * rng_signed(s);
        g->vy += m->jitter * rng_signed(s);
    }

    if (supported) {
        float dxn = (float)DIR_X[s->down], dyn = (float)DIR_Y[s->down];
        float inv = (DIR_X[s->down] && DIR_Y[s->down]) ? 0.70710678f : 1.0f;
        dxn *= inv;
        dyn *= inv;
        float vn = g->vx * dxn + g->vy * dyn;
        float tx = g->vx - vn * dxn, ty = g->vy - vn * dyn;
        g->vx -= m->contact_friction * tx;
        g->vy -= m->contact_friction * ty;
    }

    float sp2 = sim_grain_speed2(g);
    if (sp2 <= s->friction * s->friction) {
        g->vx = 0.0f;
        g->vy = 0.0f;
        return;
    }
    float inv_sp = rsqrt(sp2);
    float sp = sp2 * inv_sp;
    float k = 1.0f - s->friction * inv_sp;
    if (sp * k > m->max_speed) {
        k = m->max_speed * inv_sp;
    }
    g->vx *= k;
    g->vy *= k;
    s->motion_acc += sp * k;

    move_liquid(s, g, i, cx, cy);
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

    if (coherent(s->mat)) {
        step_liquid(s, g, i, cx, cy);
    } else {
        step_grain(s, g, cx, cy);
    }

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

// ---- Liquid levelling ---------------------------------------------------------------
//
// In a one-grain-per-cell grid only the surface layer of a body can flow, so
// after a big tilt liquid would take many seconds to level. Real liquid levels
// through pressure, moving mass underneath the surface. This pass stands in
// for that: it splits the grid into lines along gravity ("columns"), and each
// step moves up to level_rate top grains from the highest columns to the free
// spot above the lowest column within level_range lines, but only when that
// lowers the grain by more than one cell. Heights use true gravity, so tilt
// within the snapped frame is honoured. Once every column is within a cell of
// level nothing moves and the pool can sleep.

static inline float potential(const sim_t *s, int x, int y)
{
    return ((float)x + 0.5f) * s->up_x + ((float)y + 0.5f) * s->up_y;
}

#define WAVE_FALLING 0.6f    // cells/step along gravity: a grain moving faster is still falling in
#define WAVE_SRC_SPEED2 0.25f // waves move only top grains slower than 0.5 cells/step (see wave_pass)

// Walk a column along gravity from (x, y) to its top grain and fill in `c`.
static HOT_FN void column_scan(sim_t *s, sim_column_t *c, int x, int y)
{
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    int px = -1, py = -1; // last free cell
    int fx = -1, fy = -1, fpx = -1, fpy = -1; // first falling grain skipped, and the free cell above it
    for (;;) {
        while (in_bounds(s, x, y) && s->cell[y * s->w + x] == 0) {
            px = x;
            py = y;
            x += dx;
            y += dy;
        }
        // Waves look at the top of the body: skip airborne grains (a free cell
        // right below them), spray that has not come to rest yet and grains
        // still falling onto a body (a pour touching the body is one unbroken
        // line of grains, which would read as a towering column). They join
        // the body on their own and only add mass where they settle. If the
        // whole line is falling (the body itself moving), its top counts.
        if (s->mat->wave_gain <= 0.0f || !in_bounds(s, x, y)) {
            if (fx >= 0 && !in_bounds(s, x, y)) {
                x = fx;
                y = fy;
                px = fpx;
                py = fpy;
            }
            break;
        }
        const int id = s->cell[y * s->w + x] - 1;
        const sim_grain_t *g = &s->grains[id];
        const int airborne = is_free(s, x + dx, y + dy) || (s->ground[id] & LIQ_SPRAY);
        const int falling = g->vx * (float)dx + g->vy * (float)dy >= WAVE_FALLING;
        if (!airborne && !falling) {
            break;
        }
        if (!airborne && fx < 0) {
            fx = x;
            fy = y;
            fpx = px;
            fpy = py;
        }
        px = -1; // the cell above the body top is that grain, not free
        x += dx;
        y += dy;
    }
    c->top_x = (int16_t)x;
    c->top_y = (int16_t)y;
    c->source = 0;
    if (in_bounds(s, x, y)) {
        int id = s->cell[y * s->w + x] - 1;
        c->pot_top = potential(s, x, y);
        if (s->mat->wave_gain > 0.0f) {
            // Waves move any top grain that sits on something and is not
            // flying (droplets have free cells below them).
            c->source = sim_grain_speed2(&s->grains[id]) < WAVE_SRC_SPEED2;
        } else {
            c->source = sim_grain_speed2(&s->grains[id]) < s->rest_speed2;
        }
    } else {
        c->pot_top = potential(s, x, y); // empty: one past the far end
    }
    c->pot_spot = px >= 0 ? potential(s, px, py) : 3.4e38f;
}

// Columns start at the edge cells whose predecessor along gravity lies outside
// the grid. Fills s->cols and returns how many there are. Their `lat` values
// form one contiguous range (one column per line across gravity).
static HOT_FN int build_columns(sim_t *s)
{
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    const int lat = (s->down + 2) & 7;
    const int sx = DIR_X[lat], sy = DIR_Y[lat];
    int nc = 0;
    for (int e = 0; e < 2 * s->w + 2 * (s->h - 2); e++) {
        int x, y;
        if (e < s->w) {
            x = e, y = 0;
        } else if (e < 2 * s->w) {
            x = e - s->w, y = s->h - 1;
        } else if (e < 2 * s->w + s->h - 2) {
            x = 0, y = e - 2 * s->w + 1;
        } else {
            x = s->w - 1, y = e - (2 * s->w + s->h - 2) + 1;
        }
        if (in_bounds(s, x - dx, y - dy)) {
            continue;
        }
        sim_column_t *c = &s->cols[nc++];
        c->lat = (int16_t)(x * sx + y * sy);
        column_scan(s, c, x, y);
    }
    return nc;
}

// Move the top grain of column a onto the free spot above column b's top and
// refresh both entries. `vx, vy` is the velocity the moved grain gets.
static HOT_FN void column_transfer(sim_t *s, sim_column_t *a, sim_column_t *b, float vx, float vy)
{
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    int fx = a->top_x, fy = a->top_y;
    int tx = b->top_x - dx, ty = b->top_y - dy;
    int id = s->cell[fy * s->w + fx] - 1;
    cell_move(s, fx, fy, tx, ty);
    sim_grain_t *g = &s->grains[id];
    g->x = (float)tx + 0.5f;
    g->y = (float)ty + 0.5f;
    g->vx = vx;
    g->vy = vy;
    if (s->idle[id] >= SLEEP_STEPS) {
        s->sleeping--;
    }
    s->idle[id] = 0;
    s->n_transfers++;
    wake_around(s, fx, fy);

    column_scan(s, a, fx, fy);
    b->top_x = (int16_t)tx;
    b->top_y = (int16_t)ty;
    b->pot_top = potential(s, tx, ty);
    b->source = 1;
    int ux = tx - dx, uy = ty - dy;
    b->pot_spot = in_bounds(s, ux, uy) && s->cell[uy * s->w + ux] == 0 ? potential(s, ux, uy) : 3.4e38f;
}

// Liquid waves: a 1-D shallow-water model on the column heights. Every pair of
// neighbouring columns carries a flow that grows with their height difference
// (wave_gain), is damped each step (wave_damping) and persists between steps;
// whenever it reaches a whole grain, the top grain of one column moves onto
// its neighbour. Because the flow has inertia the surface overshoots level and
// rocks back, and every move is to the next column, so it reads as surface
// flow. The drive compares the mean height of wave_window columns on each side
// of the pair. Slopes too gentle for that are left to level_pass (quiet
// background).
//
// It always comes to rest:
//  - Only columns that can still take a grain carry pressure. A column filled
//    to the edge of the grid (in a diagonal frame, a line that starts at a
//    side wall) can never level, and counting it kept a permanent imbalance
//    driving the flow forever.
//  - Dead band: height differences of up to a cell drive nothing. Once the
//    whole surface has stayed inside it for WAVE_DEAD_STEPS (so not while an
//    overshoot is passing through level), the remaining flow is drained by
//    WAVE_DEAD_KEEP per step until it stops.
//  - A move never lifts a grain more than WAVE_MAX_LIFT steps along gravity
//    (a level surface needs about one: the top of one column onto its
//    neighbour). Before, single moves lifted grains up to ~70 cells onto
//    stray "column tops" far above the body, gravity dropped them straight
//    back, and the waves lifted them again: the self-sustaining bobbing.
//  - Only top grains slower than sqrt(WAVE_SRC_SPEED2) are moved, so the
//    waves do not push mass the particle body is already carrying (both
//    moving the same mass, each with its own inertia, made sloshing grow).
//  - A pair whose next move is impossible (no room in the receiving column,
//    too steep) loses its flow at once; one whose top grain is still moving
//    holds and loses a fifth of it per step.
//  - The waves stop after WAVE_STILL_STEPS passes without a move, or once the
//    surface has stayed within the dead band for 2 x WAVE_DEAD_STEPS; the
//    quiet level_pass (which only ever lowers grains) finishes the rest.
//  - Backstop: if the largest imbalance has reversed WAVE_BACKSTOP_FLIPS
//    times since the last input, the waves are still moving
//    WAVE_BACKSTOP_STEPS after it, and their flow energy has not fallen by
//    WAVE_DECAY_MIN over the last second (a real slosh dies away faster), the
//    surface is oscillating on its own: the flow is damped hard. So it is
//    when waves are still moving WAVE_BACKSTOP_LATE after the last input
//    whatever they are doing (slow churning in an odd pose); the quiet
//    level_pass, which only ever lowers grains, then finishes. Both are
//    counted in n_wave_backstop.
// Droplets, spray not yet at rest and grains still falling in are not part
// of the surface (column_scan): landings only add mass where they settle.
// Returns the number of grains moved.
#define WAVE_DEAD_KEEP 0.85f
#define WAVE_DEAD_STEPS (SIM_HZ / 2)
#define WAVE_STILL_STEPS 30
#define WAVE_BACKSTOP_STEPS (2 * SIM_HZ)
#define WAVE_BACKSTOP_FLIPS 3
#define WAVE_BACKSTOP_LATE (4 * SIM_HZ)
#define WAVE_MAX_LIFT 4.0f
#define WAVE_DECAY_MIN 0.3f

// Free spot above the column, and a top that can be counted as its height.
static inline int wave_wet(const sim_t *s, const sim_column_t *c)
{
    return in_bounds(s, c->top_x, c->top_y) && c->pot_spot < 3.0e38f;
}

static HOT_FN int wave_pass(sim_t *s)
{
    const material_t *m = s->mat;
    const int nc = build_columns(s);
    int lat_min = s->cols[0].lat;
    for (int i = 1; i < nc; i++) {
        lat_min = s->cols[i].lat < lat_min ? s->cols[i].lat : lat_min;
    }
    for (int i = 0; i < nc; i++) {
        s->col_order[s->cols[i].lat - lat_min] = (int16_t)i;
    }
    if (s->flux_down != s->down) {
        memset(s->flux, 0, (size_t)(s->w + s->h) * sizeof(float));
        memset(s->wave_acc, 0, (size_t)(s->w + s->h) * sizeof(float));
        s->flux_down = s->down;
    }

    // Moved grains get a token velocity along the flow so the glow shows it.
    const int lat = (s->down + 2) & 7;
    const float ts = (DIR_X[lat] && DIR_Y[lat]) ? s->token * 0.7071f : s->token;
    const float tvx = (float)DIR_X[lat] * ts, tvy = (float)DIR_Y[lat] * ts;

    // Most a move may lift a grain: WAVE_MAX_LIFT steps along gravity (a
    // diagonal step is 1.41 cells high).
    const float lift_max = WAVE_MAX_LIFT * ((s->down & 1) ? 1.41421356f : 1.0f);
    int moved = 0;
    float fmax = 0.0f;
    const int win = m->wave_window > 0 ? m->wave_window : 1;
    // Prefix sums of the heights of the columns that carry pressure (hold
    // liquid and have room above it), and of how many there are (start of
    // the step). Empty columns carry no liquid: in a diagonal frame their
    // nominal "floor" can be far up a wall, which would push the wrong way.
    float *pre = s->wave_pre;
    int16_t *cnt = s->wave_cnt;
    pre[0] = 0.0f;
    cnt[0] = 0;
    for (int k = 0; k < nc; k++) {
        const sim_column_t *c = &s->cols[s->col_order[k]];
        const int wet = wave_wet(s, c);
        pre[k + 1] = pre[k] + (wet ? c->pot_top : 0.0f);
        cnt[k + 1] = (int16_t)(cnt[k] + wet);
    }
    // Waves start only when the tank is clearly out of level (wave_start
    // cells between the halves around some pair) and run until they die out;
    // smaller imbalances such as hand sway are left to the quiet levelling.
    if (!s->wave_active) {
        float dmax = 0.0f;
        for (int k = 0; k + 1 < nc; k++) {
            const int a0 = k + 1 - win > 0 ? k + 1 - win : 0;
            const int b1 = k + 1 + win < nc ? k + 1 + win : nc;
            const int na = cnt[k + 1] - cnt[a0], nb = cnt[b1] - cnt[k + 1];
            if (na && nb) {
                float d = fabsf((pre[k + 1] - pre[a0]) / (float)na - (pre[b1] - pre[k + 1]) / (float)nb);
                dmax = d > dmax ? d : dmax;
            }
        }
        if (dmax < m->wave_start) {
            s->wave_quiet = 1;
            return -nc - 1; // idle; the columns are built for level_pass
        }
        s->wave_active = 1;
        s->wave_still = 0;
        s->wave_dead = 0;
        s->wave_e_ref = 0.0f;
        s->wave_decaying = 1;
        s->wave_backstop = 0;
    }
    // Backstop: oscillating on its own long after the last input.
    if (s->steps - s->input_step <= 1) {
        s->wave_flips = 0;
    }
    const uint32_t since = s->steps - s->input_step;
    const int backstop = (since > WAVE_BACKSTOP_STEPS && s->wave_flips >= WAVE_BACKSTOP_FLIPS && !s->wave_decaying) ||
                         since > WAVE_BACKSTOP_LATE;
    const int drain = s->wave_dead >= WAVE_DEAD_STEPS;
    int all_dead = 1;
    float dbig = 0.0f;

    // Alternate the sweep direction so neither side is favoured.
    const int rev = (int)(s->steps & 1);
    for (int j = 0; j + 1 < nc; j++) {
        const int k = rev ? nc - 2 - j : j;
        sim_column_t *a = &s->cols[s->col_order[k]];
        sim_column_t *b = &s->cols[s->col_order[k + 1]];
        // Both columns must hold liquid; then compare the mean height of the
        // pressure-carrying columns among up to `win` on each side of the pair.
        const int wa = in_bounds(s, a->top_x, a->top_y), wb = in_bounds(s, b->top_x, b->top_y);
        if (!wa || !wb) {
            // Edge of the liquid: spill into a dry neighbour whose floor is
            // really lower (true gravity), so a heap's foot spreads out. No
            // pressure or momentum here; up-the-wall "floors" of empty corner
            // lines in a diagonal frame are high and never qualify.
            s->flux[k] = 0.0f;
            s->wave_acc[k] = 0.0f;
            sim_column_t *wet = wa ? a : wb ? b : NULL, *dry = wa ? b : a;
            for (int t = 0; wet && t < m->level_rate && wet->source && dry->pot_spot < 3.0e38f &&
                            wet->pot_top - dry->pot_spot >= 1.0f;
                 t++) {
                column_transfer(s, wet, dry, wet == a ? tvx : -tvx, wet == a ? tvy : -tvy);
                moved++;
            }
            continue;
        }
        const int a0 = k + 1 - win > 0 ? k + 1 - win : 0;
        const int b1 = k + 1 + win < nc ? k + 1 + win : nc;
        const int na = cnt[k + 1] - cnt[a0], nb = cnt[b1] - cnt[k + 1];
        const float d = na && nb ? (pre[k + 1] - pre[a0]) / (float)na - (pre[b1] - pre[k + 1]) / (float)nb : 0.0f;
        const int dead = d <= 1.0f && d >= -1.0f;
        all_dead &= dead;
        dbig = fabsf(d) > fabsf(dbig) ? d : dbig;
        // (The backstop cuts the drive and drains what flow is left.)
        const float drive = backstop ? 0.0f : d > 1.0f ? d - 1.0f : d < -1.0f ? d + 1.0f : 0.0f;
        // Flow velocity (grains/step): accelerated by the height difference,
        // damped, at most level_rate (+ a quarter of the imbalance: tall
        // heaps slump faster). It keeps going after the surface passes level,
        // which is the overshoot. The accumulator turns it into moves.
        const float qcap = (float)m->level_rate + 0.25f * fabsf(d);
        float q = (s->flux[k] + m->wave_gain * drive) * (1.0f - m->wave_damping);
        if (dead && drain) {
            q *= WAVE_DEAD_KEEP;
        }
        if (backstop) {
            q *= 0.5f;
        }
        q = q > qcap ? qcap : q < -qcap ? -qcap : q;
        float acc = s->wave_acc[k] + q;
        if (fabsf(q) < 0.02f && ((dead && drain) || backstop)) {
            q = 0.0f;
            acc = 0.0f;
        }
        while (acc >= 1.0f || acc <= -1.0f) {
            const int fwd = acc > 0.0f;
            sim_column_t *from = fwd ? a : b, *to = fwd ? b : a;
            const float lift = to->pot_spot - from->pot_top;
            if (to->pot_spot >= 3.0e38f || lift > lift_max) {
                // No room in the receiving column, or it would lift the grain
                // more than WAVE_MAX_LIFT steps: the flow at this pair stops.
                q = 0.0f;
                acc = 0.0f;
                break;
            }
            if (!from->source) {
                // The top is still moving (or a droplet): hold, losing some flow.
                acc = fwd ? 0.99f : -0.99f;
                q *= 0.8f;
                break;
            }
            column_transfer(s, from, to, fwd ? tvx : -tvx, fwd ? tvy : -tvy);
            acc += fwd ? -1.0f : 1.0f;
            moved++;
        }
        s->flux[k] = q;
        s->wave_acc[k] = acc;
        fmax = fabsf(q) > fmax ? fabsf(q) : fmax;
    }
    // Once a second: is the flow energy dying away?
    if (s->steps % SIM_HZ == 0) {
        float e = 0.0f;
        for (int k = 0; k + 1 < nc; k++) {
            e += s->flux[k] * s->flux[k];
        }
        s->wave_decaying = e < (1.0f - WAVE_DECAY_MIN) * s->wave_e_ref;
        s->wave_e_ref = e;
    }
    if (backstop && moved && !s->wave_backstop) {
        s->wave_backstop = 1;
        s->n_wave_backstop++;
    }
    s->wave_still = moved ? 0 : s->wave_still + 1;
    s->wave_dead = all_dead ? s->wave_dead + 1 : 0;
    if (dbig > 1.0f || dbig < -1.0f) {
        const int sg = dbig > 0.0f ? 1 : -1;
        s->wave_flips += s->wave_sign && sg != s->wave_sign;
        s->wave_sign = sg;
    }
    s->wave_quiet = (!moved && (fmax < 0.02f || s->wave_still >= WAVE_STILL_STEPS)) ||
                    s->wave_dead >= 2 * WAVE_DEAD_STEPS;
    if (s->wave_quiet) {
        s->wave_active = 0;
        memset(s->flux, 0, (size_t)(s->w + s->h) * sizeof(float));
        memset(s->wave_acc, 0, (size_t)(s->w + s->h) * sizeof(float));
    }
    return moved;
}

// nc: columns already built in s->cols by the caller, or < 0 to build them.
static HOT_FN void level_pass(sim_t *s, int rate, int nc)
{
    const material_t *m = s->mat;
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    if (nc < 0) {
        nc = build_columns(s);
    }

    for (int t = 0; t < rate; t++) {
        // Highest movable top.
        int src = -1;
        for (int i = 0; i < nc; i++) {
            if (s->cols[i].source && (src < 0 || s->cols[i].pot_top > s->cols[src].pot_top)) {
                src = i;
            }
        }
        if (src < 0) {
            s->level_settled = 1;
            return;
        }
        // Lowest free spot within range.
        sim_column_t *a = &s->cols[src];
        int dst = -1;
        for (int i = 0; i < nc; i++) {
            int d = s->cols[i].lat - a->lat;
            if (i != src && d <= m->level_range && d >= -m->level_range &&
                (dst < 0 || s->cols[i].pot_spot < s->cols[dst].pot_spot)) {
                dst = i;
            }
        }
        // Moving lowers the grain by a->pot_top - pot_spot. Requiring at least
        // half a cell makes every move strictly reduce the total height, so
        // the process always settles (no grains trading places back and forth).
        if (dst < 0 || a->pot_top - s->cols[dst].pot_spot < 0.5f) {
            s->level_settled = 1; // level to within about a cell: done
            return;
        }
        sim_column_t *b = &s->cols[dst];

        // Move the top grain of `a` to the spot above `b`'s top.
        int fx = a->top_x, fy = a->top_y;
        int tx = b->top_x - dx, ty = b->top_y - dy;
        int id = s->cell[fy * s->w + fx] - 1;
        cell_move(s, fx, fy, tx, ty);
        sim_grain_t *g = &s->grains[id];
        g->x = (float)tx + 0.5f;
        g->y = (float)ty + 0.5f;
        g->vx = g->vy = 0.0f;
        s->idle[id] = 0;
        s->n_transfers++;
        wake_around(s, fx, fy);

        // Refresh both columns from the cell just above their old top.
        column_scan(s, a, fx, fy);
        b->top_x = (int16_t)tx;
        b->top_y = (int16_t)ty;
        b->pot_top = potential(s, tx, ty);
        b->source = 1;
        int ux = tx - dx, uy = ty - dy;
        b->pot_spot = in_bounds(s, ux, uy) && s->cell[uy * s->w + ux] == 0 ? potential(s, ux, uy) : 3.4e38f;
    }
}

// Liquid update order: awake grains farthest along gravity first (bucket sort
// on their line across gravity), so a grain is moved before the ones behind
// it and a falling body falls together instead of opening a gap behind every
// grain that happened to be updated before the grain ahead of it. Within a
// line the order still starts at a random grain and alternates direction.
// Grains woken during the step wait for the next one.
static HOT_FN void liquid_sweep(sim_t *s, int start, int reverse)
{
    const int n = s->n, w = s->w, h = s->h;
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    const int off = (w - 1) + (h - 1);   // keys dx*x + dy*y + off are in 0 .. 2 * off
    const int nb = 2 * off + 1;
    uint16_t *cnt = s->order_cnt;
    memset(cnt, 0, (size_t)nb * sizeof(uint16_t));
    int awake = 0;
    for (int i = 0; i < n; i++) {
        if (s->idle[i] < SLEEP_STEPS) {
            const sim_grain_t *g = &s->grains[i];
            cnt[dx * (int)g->x + dy * (int)g->y + off]++;
            awake++;
        }
    }
    if (awake == 0) {
        return;
    }
    // Bucket starts, highest key (farthest along gravity) first.
    int pos = 0;
    for (int k = nb - 1; k >= 0; k--) {
        int c = cnt[k];
        cnt[k] = (uint16_t)pos;
        pos += c;
    }
    int idx = start;
    for (int c = 0; c < n; c++) {
        if (s->idle[idx] < SLEEP_STEPS) {
            const sim_grain_t *g = &s->grains[idx];
            s->order[cnt[dx * (int)g->x + dy * (int)g->y + off]++] = (uint16_t)idx;
        }
        if (reverse) {
            idx = (idx == 0) ? n - 1 : idx - 1;
        } else {
            idx = (idx + 1 == n) ? 0 : idx + 1;
        }
    }
    for (int c = 0; c < awake; c++) {
        update_grain(s, s->order[c]);
    }
}

HOT_FN void sim_step(sim_t *s)
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
    if (coherent(s->mat) && s->down >= 0) {
        liquid_sweep(s, start, reverse);
    } else {
        int idx = start;
        for (int c = 0; c < n; c++) {
            update_grain(s, idx);
            if (reverse) {
                idx = (idx == 0) ? n - 1 : idx - 1;
            } else {
                idx = (idx + 1 == n) ? 0 : idx + 1;
            }
        }
    }

    // Liquid levelling. Skipped while it cannot find anything new to do: the
    // last pass ended level, no grain has changed cell since and gravity has
    // turned less than LEVEL_RECHECK_DEG; also skipped while the liquid is
    // splashing hard (motion above level_max_motion), when it only costs time.
    // (It must not depend on grains being awake: levelling moves can finish
    // long after the grains around them have gone back to sleep.)
    const material_t *m = s->mat;
    if (m->level_rate > 0 && s->down >= 0 && s->motion_acc < m->level_max_motion) {
        int same_dir = s->level_up_x * s->up_x + s->level_up_y * s->up_y > s->level_recheck_cos;
        if (!(s->level_settled && s->moves == s->level_seen_moves && same_dir)) {
            s->level_settled = 0;
            s->level_up_x = s->up_x;
            s->level_up_y = s->up_y;
            if (m->wave_gain > 0.0f) {
                // Waves first; when they have nothing to do, the quiet global
                // pass finishes slopes too gentle for them.
                const int r = wave_pass(s);
                if (r <= 0 && m->level_global_rate > 0) {
                    level_pass(s, m->level_global_rate, r < 0 ? -r - 1 : -1);
                } else {
                    s->level_settled = m->level_global_rate == 0;
                }
                s->level_settled = s->level_settled && s->wave_quiet;
            } else {
                level_pass(s, m->level_rate, -1);
            }
            s->level_seen_moves = s->moves;
        }
    }

    s->motion = s->motion_acc;
    s->steps++;
}
