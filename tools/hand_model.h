// Hand-motion model shared by the host tools (sim_hand, sim_idle): the board
// held leaned back HAND_LEAN degrees with +/-0.03 g white tremor on every axis
// at IMU_POLL_HZ, an in-plane sway of +/-3 degrees at 0.25 Hz plus a slow +/-2
// degree drift at 0.07 Hz, and a +/-2 degree lean wobble at 0.3 Hz, all through
// the firmware's gravity low-pass (IMU_LPF_ALPHA). Include after config.h with
// DEG and HAND_LEAN defined.
#pragma once

#include <math.h>
#include <string.h>

static float urand(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (float)((*seed >> 8) & 0xFFFF) / 32767.5f - 1.0f;
}

typedef struct {
    float lp[3];
    double t;
    unsigned seed;
    float extra_rot; // deliberate rotation added on top of the hand motion
} hand_t;

// Advance the simulated IMU by one sim step and return filtered gravity.
static void hand_step(hand_t *h, float out[3])
{
    const double imu_dt = 1.0 / IMU_POLL_HZ;
    double end = h->t + 1.0 / SIM_HZ;
    while (h->t < end) {
        float t = (float)h->t;
        float rot = 3.0f * sinf(t * 2.0f * 3.14159265f * 0.25f) + 2.0f * sinf(t * 2.0f * 3.14159265f * 0.07f) +
                    h->extra_rot;
        float lean = HAND_LEAN + 2.0f * sinf(t * 2.0f * 3.14159265f * 0.3f);
        float in_plane = cosf(lean * DEG);
        float g[3] = {
            in_plane * sinf(rot * DEG) + 0.03f * urand(&h->seed),
            in_plane * cosf(rot * DEG) + 0.03f * urand(&h->seed),
            -sinf(lean * DEG) + 0.03f * urand(&h->seed),
        };
        for (int i = 0; i < 3; i++) {
            h->lp[i] += IMU_LPF_ALPHA * (g[i] - h->lp[i]);
        }
        h->t += imu_dt;
    }
    memcpy(out, h->lp, sizeof(h->lp));
}
