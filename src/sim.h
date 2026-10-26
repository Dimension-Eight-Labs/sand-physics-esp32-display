// Falling-sand simulation: float grains on an occupancy grid.
//
// Hardware-free (C standard library only) so it can be built and tested on a
// PC; see tools/sim_host.c. All tunables come from config.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    float x, y;     // position in cell units; the grain occupies cell (int)x, (int)y
    float vx, vy;   // velocity in cells per step
} sim_grain_t;

typedef struct {
    int w, h;            // grid size in cells
    int n;               // number of grains
    sim_grain_t *grains;
    uint8_t *shade;      // per grain, 0..n_shades-1
    uint16_t *cell;      // w*h, grain index + 1, 0 = empty
    uint8_t *idle;       // per grain: steps without moving; >= SLEEP_STEPS = asleep

    // Derived from the last sim_set_gravity() call.
    float ax, ay;        // in-plane acceleration, cells/step^2
    float friction;      // Coulomb friction against the screen face, cells/step^2
    int down;            // snapped gravity direction 0..7, or -1 if the sand is held by friction
    float repose_tan[2]; // tan of slide threshold toward lateral side -1 / +1
    float repose_prob;   // chance per step that a resting grain tries the repose rule
    float ref_g[3];      // gravity when grains were last woken (see sim_set_gravity)
    int ref_down;

    uint32_t rng;
    uint32_t steps;
    float motion;        // sum of grain speeds over the last step
    float motion_acc;    // running sum during a step
    int awake;           // grains that were updated (not asleep) in the last step
    int sleeping;        // grains currently asleep
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
