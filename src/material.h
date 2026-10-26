// Material table access. Hardware-free.
#pragma once

#include "config.h"

// idx is clamped to a valid material.
const material_t *material_get(int idx);

