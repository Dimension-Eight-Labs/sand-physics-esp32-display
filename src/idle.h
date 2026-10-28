// Idle dimming and auto-off: decides when the board has been left alone (for
// battery use), how bright the backlight and LED should be, and when to power
// off. Hardware-free, so the host
// tools can check it against the hand-motion model (tools/sim_idle.c).
//
// Activity is a knock (any shake event), gravity turning more than
// IDLE_WAKE_TILT_DEG away from where it was at the last activity (a real
// tilt; hand tremor and sway stay well inside it), sand still visibly moving
// (total motion above IDLE_WAKE_MOTION), or a button press (idle_poke()).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float ref[3];       // gravity at the last activity (unit vector)
    int64_t last_us;    // time of the last activity
    bool have_ref;
} idle_t;

void idle_init(idle_t *d, int64_t now_us);

// Feed one gravity reading (g, screen frame), whether a shake happened and the
// simulation's total motion. Returns true if this counts as activity (and
// records it).
bool idle_update(idle_t *d, const float g[3], bool shake, float motion, int64_t now_us);

// Activity from elsewhere (button).
void idle_poke(idle_t *d, int64_t now_us);

// Brightness factor for `idle_us` without activity: 0 = full, 1 = fully
// dimmed. Fades in over IDLE_DIM_FADE_MS after IDLE_DIM_AFTER_S; always 0 when
// IDLE_DIM_ENABLE is 0.
float idle_dim_amount(int64_t idle_us);

// True once `idle_us` without activity reaches AUTO_OFF_S (never when
// AUTO_OFF_ENABLE is 0).
bool idle_auto_off(int64_t idle_us);
