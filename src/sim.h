// Falling-sand simulation: float grains on an occupancy grid.
//
// Hardware-free (C standard library only) so it can be built and tested on a
// PC; see tools/sim_host.c. All tunables come from config.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct {
    float x, y;     // position in cell units; the grain occupies cell (int)x, (int)y
    float vx, vy;   // velocity in cells per step
} sim_grain_t;

// One line of cells along the gravity direction (liquid levelling).
typedef struct {
    int16_t top_x, top_y; // topmost grain, or one past the far end if empty
    int16_t lat;          // position across gravity, for the range limit
    int16_t source;       // top grain may be moved (resting liquid)
    float pot_top;        // height of the top grain along true "up"
    float pot_spot;       // height of the free cell above it (FLT_MAX if none)
} sim_column_t;

// Liquid per-grain flags (sim_t.ground).
#define LIQ_GROUND 1u // rests on a still chain down to a wall
#define LIQ_SPRAY  2u // thrown by a knock: absorbed when it lands, never re-thrown; cleared at rest

typedef struct {
    int w, h;            // grid size in cells
    int n;               // number of grains
    sim_grain_t *grains;
    uint8_t *shade;      // per grain, 0..n_shades-1
    uint16_t *cell;      // w*h, grain index + 1, 0 = empty
    uint8_t *idle;       // per grain: steps without moving; >= SLEEP_STEPS = asleep
    sim_column_t *cols;  // w + h entries, scratch for liquid levelling
    uint8_t *ground;     // liquid: LIQ_GROUND | LIQ_SPRAY flags per grain
    float *flux;         // liquid waves: flow velocity between neighbouring columns, grains/step (w + h)
    float *wave_acc;     // liquid waves: flow accumulated toward the next whole grain (w + h)
    int16_t *col_order;  // liquid waves: column index by position across gravity
    float *wave_pre;     // liquid waves: prefix sums of wet column heights (w + h + 1)
    int16_t *wave_cnt;   // liquid waves: prefix counts of wet columns (w + h + 1)
    int flux_down;       // gravity direction the flux belongs to
    int wave_quiet;      // last wave pass moved nothing and its flow had died out
    int wave_active;     // waves running (started by a clear imbalance, until they die out)
    int wave_still;      // consecutive wave passes that moved nothing
    int wave_dead;       // consecutive wave passes with the whole surface inside the dead band
    int wave_backstop;   // the backstop is damping the current wave episode
    int wave_sign;       // sign of the largest imbalance last pass
    int wave_flips;      // times it reversed since the last input (oscillation count)
    float wave_e_ref;    // wave flow energy (sum q^2) at the last once-a-second check
    int wave_decaying;   // that energy fell by WAVE_DECAY_MIN or more over the last second
    uint32_t input_step; // step of the last input (shake, gravity wake, material, spawn)
    uint16_t *order;     // liquid: awake grains this step, farthest along gravity first (n)
    uint16_t *order_cnt; // liquid: bucket counts for that sort (2 * (w + h))
    int knock_left;      // liquid: grains the current knock may still throw
    uint32_t knock_start; // step the current knock's budget started

    // Active material and values derived from it (sim_set_material).
    const material_t *mat;
    float rest_speed2;   // rest_speed^2
    float damp_keep;     // 1 - damping
    float token;         // rule_token * rest_speed
    int wake_radius;     // material wake_radius, or WAKE_RADIUS

    // Derived from the last sim_set_gravity() call.
    float ax, ay;        // in-plane acceleration, cells/step^2
    float friction;      // Coulomb friction against the screen face, cells/step^2
    int down;            // snapped gravity direction 0..7, or -1 if the sand is held by friction
    float repose_tan[2]; // REPOSE: tan of slide threshold toward lateral side -1 / +1
    float tilt_tan[2];   // SPREAD: height lost per lateral cell toward side -1 / +1
    float up_x, up_y;    // unit vector opposite to in-plane gravity (liquid levelling)
    float rule_prob;     // chance per step that a resting grain applies its rule
    float last_g[3];     // last gravity input
    float ref_g[3];      // gravity when grains were last woken (see sim_set_gravity)
    int ref_down;
    float wake_cos;      // cos(WAKE_ANGLE_DEG)

    uint32_t rng;
    uint32_t steps;
    float motion;        // sum of grain speeds over the last step
    float motion_acc;    // running sum during a step
    int awake;           // grains that were updated (not asleep) in the last step
    int sleeping;        // grains currently asleep
    // Diagnostics (running totals; never affect the physics).
    uint32_t n_wake_all;  // wake-everyone events (gravity, shake, material, spawn)
    uint32_t n_wake_exposed; // liquid: gravity changes that woke only exposed grains
    uint32_t moves;       // grains that changed cell (running total)
    // Liquid levelling bookkeeping: skip the pass while nothing can change.
    uint32_t level_seen_moves;
    int level_settled;    // last pass found the surface level
    float level_up_x, level_up_y; // gravity direction of that pass
    float level_recheck_cos;      // cos(LEVEL_RECHECK_DEG)
    uint32_t n_transfers; // liquid levelling moves
    uint32_t n_pushes;    // liquid spread pushes
    uint32_t n_knocks;    // liquid knock splashes (impulses within 6 steps of each other count once)
    uint32_t knock_step;  // step of the last knock impulse
    uint32_t n_knock_grains; // liquid grains thrown by knocks
    uint32_t n_spray_landings; // spray grains absorbed on landing
    uint32_t n_landings;  // liquid landings (faster than rest_speed) absorbed into the body
    uint32_t n_wave_backstop; // liquid: times the wave backstop had to calm a self-sustained oscillation
} sim_t;

// Bytes of working memory sim_init() needs for a w x h grid with n grains.
size_t sim_mem_bytes(int w, int h, int n);

// Initialise into caller-provided memory (at least sim_mem_bytes(), any
// alignment from malloc is fine). Grains are spawned via sim_spawn().
void sim_init(sim_t *s, void *mem, int w, int h, int n, uint32_t seed);

// Scatter all grains randomly over the top `region` fraction of the grid
// (0..1) with small random velocities and random shades 0..n_shades-1.
void sim_spawn(sim_t *s, float region, int n_shades);

// Gravity in g, screen frame: x right, y down, z out of the screen.
void sim_set_gravity(sim_t *s, float gx, float gy, float gz);

// Switch material; every grain stays where it is and is woken. sim_init()
// starts with MATERIAL_CLASSIC.
void sim_set_material(sim_t *s, const material_t *m);

// Kick every grain: directional part along (dir_x, dir_y) (any length, it is
// normalised; zero for none) plus an isotropic random part, both scaled by
// `strength` in cells/step.
void sim_shake(sim_t *s, float dir_x, float dir_y, float strength);

// Advance one fixed timestep.
void sim_step(sim_t *s);

static inline float sim_grain_speed2(const sim_grain_t *g)
{
    return g->vx * g->vx + g->vy * g->vy;
}
