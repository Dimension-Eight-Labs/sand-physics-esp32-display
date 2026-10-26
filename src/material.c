// The material table; parameter values live in config.h.
#include "material.h"

static const material_t MATERIALS[MATERIAL_COUNT] = {
    [MATERIAL_CLASSIC] = MATERIAL_CLASSIC_PARAMS,
    [MATERIAL_LIQUID] = MATERIAL_LIQUID_PARAMS,
};

const material_t *material_get(int idx)
{
    if (idx < 0 || idx >= MATERIAL_COUNT) {
        idx = MATERIAL_CLASSIC;
    }
    return &MATERIALS[idx];
}

float material_max_speed(void)
{
    float m = 0.0f;
    for (int i = 0; i < MATERIAL_COUNT; i++) {
        if (MATERIALS[i].max_speed > m) {
            m = MATERIALS[i].max_speed;
        }
    }
    return m;
}
