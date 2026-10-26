// Idle dimming detector; see idle.h.
#include "idle.h"

#include <math.h>

#include "config.h"

#define IDLE_DEG2RAD (3.14159265f / 180.0f)

static void set_ref(idle_t *d, const float g[3])
{
    const float n = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (n < 1e-3f) {
        return; // no usable reading (free fall, or no sensor)
    }
    for (int i = 0; i < 3; i++) {
        d->ref[i] = g[i] / n;
    }
    d->have_ref = true;
}

void idle_init(idle_t *d, int64_t now_us)
{
    d->have_ref = false;
    d->last_us = now_us;
}

void idle_poke(idle_t *d, int64_t now_us)
{
    d->last_us = now_us;
}

bool idle_update(idle_t *d, const float g[3], bool shake, float motion, int64_t now_us)
{
    bool active = shake || motion > IDLE_WAKE_MOTION;
    const float n = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (!d->have_ref) {
        set_ref(d, g);
    } else if (n > 1e-3f) {
        const float c = (g[0] * d->ref[0] + g[1] * d->ref[1] + g[2] * d->ref[2]) / n;
        if (c < cosf(IDLE_WAKE_TILT_DEG * IDLE_DEG2RAD)) {
            active = true;
        }
    }
    if (active) {
        // The new attitude is the reference for the next tilt.
        set_ref(d, g);
        d->last_us = now_us;
    }
    return active;
}

float idle_dim_amount(int64_t idle_us)
{
#if IDLE_DIM_ENABLE
    const int64_t start = (int64_t)IDLE_DIM_AFTER_S * 1000000;
    if (idle_us <= start) {
        return 0.0f;
    }
    const float t = (float)(idle_us - start) / (IDLE_DIM_FADE_MS * 1000.0f);
    return t < 1.0f ? t : 1.0f;
#else
    (void)idle_us;
    return 0.0f;
#endif
}
