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

// Water only: a gravity change can only move grains that have somewhere to
// go, so wake just the sleepers with a free cell beside, above or below
// them. Fully surrounded grains (most of a pool) wake later through
// wake_around() if a neighbour leaves; so does the rare grain whose only
// free neighbour is diagonal. (Sand keeps wake_all() so CLASSIC stays
// bit-identical to v1.)
static HOT_FN void wake_exposed(sim_t *s)
{
    s->n_wake_exposed++;
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
    size_t cols = (size_t)(w + h) * sizeof(sim_column_t); // water levelling
    return grains + cells + 2 * bytes + cols;
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
        // Water levels perpendicular to true gravity, which leans by tilt_deg
        // from the snapped frame: moving k cells toward the side it leans to
        // lowers a grain by k * tan(tilt).
        float t = tanf(tilt_deg * DEG2RAD);
        s->tilt_tan[1] = t;
        s->tilt_tan[0] = -t;
        s->up_x = -gx / g_xy;
        s->up_y = -gy / g_xy;
        return;
    }

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
        if (s->mat->rest_rule == MAT_REST_SPREAD) {
            wake_exposed(s);
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
    s->ref_g[0] = s->last_g[0];
    s->ref_g[1] = s->last_g[1];
    s->ref_g[2] = s->last_g[2];
    s->ref_down = s->down;
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

// Spread rule (water) for a resting grain at (cx, cy): look up to spread_scan
// cells along each lateral direction (random side first) for a spot where the
// grain would end up at least one cell lower in true gravity, either by
// dropping into a free cell below that spot or because gravity leans toward
// that side. If found, push the grain that way at spread_push cells/step; it
// then slides with momentum (water has almost no contact friction), which
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
            // Water with both sideways cells taken cannot spread: skip the
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

// ---- Water levelling ---------------------------------------------------------------
//
// In a one-grain-per-cell grid only the surface layer of a body can flow, so
// after a big tilt water would take many seconds to level. Real water levels
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

// Walk a column along gravity from (x, y) to its top grain and fill in `c`.
static void column_scan(sim_t *s, sim_column_t *c, int x, int y)
{
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    int px = -1, py = -1; // last free cell
    while (in_bounds(s, x, y) && s->cell[y * s->w + x] == 0) {
        px = x;
        py = y;
        x += dx;
        y += dy;
    }
    c->top_x = (int16_t)x;
    c->top_y = (int16_t)y;
    c->source = 0;
    if (in_bounds(s, x, y)) {
        int id = s->cell[y * s->w + x] - 1;
        c->pot_top = potential(s, x, y);
        c->source = sim_grain_speed2(&s->grains[id]) < s->rest_speed2;
    } else {
        c->pot_top = potential(s, x, y); // empty: one past the far end
    }
    c->pot_spot = px >= 0 ? potential(s, px, py) : 3.4e38f;
}

static HOT_FN void level_pass(sim_t *s)
{
    const material_t *m = s->mat;
    const int dx = DIR_X[s->down], dy = DIR_Y[s->down];
    const int lat = (s->down + 2) & 7;
    const int sx = DIR_X[lat], sy = DIR_Y[lat];

    // Columns start at the edge cells whose predecessor along gravity lies
    // outside the grid (top/bottom rows, then left/right edges without corners).
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

    for (int t = 0; t < m->level_rate; t++) {
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
    int idx = start;
    for (int c = 0; c < n; c++) {
        update_grain(s, idx);
        if (reverse) {
            idx = (idx == 0) ? n - 1 : idx - 1;
        } else {
            idx = (idx + 1 == n) ? 0 : idx + 1;
        }
    }

    // Water levelling. Skipped while it cannot find anything new to do: the
    // last pass ended level, no grain has changed cell since and gravity has
    // turned less than LEVEL_RECHECK_DEG; also skipped while the water is
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
            level_pass(s);
            s->level_seen_moves = s->moves;
        }
    }

    s->motion = s->motion_acc;
    s->steps++;
}
