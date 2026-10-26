// Material overrides from the command line for the host tools: name=value
// arguments set the material_t field of that name, e.g.
//   ./sim_liquidfx wave_damping=0.05 viscosity=0.3
// Every numeric field can be set. An unknown name or a bad value is an error
// (exit 2), so a typo never silently runs the defaults.
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

typedef struct {
    const char *name;
    size_t off;
    int is_int;
} override_field_t;

#define OVR_F(f) { #f, offsetof(material_t, f), 0 }
#define OVR_I(f) { #f, offsetof(material_t, f), 1 }

static const override_field_t OVERRIDE_FIELDS[] = {
    OVR_F(gravity_scale), OVR_F(damping), OVR_F(elasticity), OVR_F(max_speed), OVR_F(jitter),
    OVR_F(contact_friction), OVR_F(rest_speed), OVR_F(glass_friction), OVR_F(rule_token),
    OVR_I(rest_rule), OVR_F(rule_prob),
    OVR_F(repose_angle_deg), OVR_I(repose_scan), OVR_F(repose_diag_correction_deg), OVR_F(repose_dither),
    OVR_I(spread_scan), OVR_F(spread_push), OVR_I(spread_step), OVR_I(level_rate), OVR_F(wave_gain),
    OVR_F(wave_damping), OVR_I(wave_window), OVR_F(wave_start), OVR_I(level_global_rate), OVR_I(level_range),
    OVR_F(level_max_motion), OVR_I(wake_radius), OVR_I(flow_model),
    OVR_F(viscosity), OVR_F(impact_to_flow), OVR_F(impact_absorb), OVR_F(knock_splash), OVR_F(knock_fan_deg),
    OVR_I(knock_depth), OVR_I(knock_max), OVR_F(knock_interior), OVR_F(shade_spread),
};

// Apply one "name=value" argument to m. Returns 1 if it was one (and applied),
// 0 if the argument has no '=' (not an override). Exits on an unknown name.
static int override_apply(material_t *m, const char *arg)
{
    const char *eq = strchr(arg, '=');
    if (!eq) {
        return 0;
    }
    size_t len = (size_t)(eq - arg);
    char *end;
    double v = strtod(eq + 1, &end);
    if (end == eq + 1 || *end) {
        fprintf(stderr, "bad value in override '%s'\n", arg);
        exit(2);
    }
    for (size_t k = 0; k < sizeof(OVERRIDE_FIELDS) / sizeof(OVERRIDE_FIELDS[0]); k++) {
        const override_field_t *f = &OVERRIDE_FIELDS[k];
        if (strlen(f->name) == len && !strncmp(f->name, arg, len)) {
            if (f->is_int) {
                *(int *)((char *)m + f->off) = (int)v;
            } else {
                *(float *)((char *)m + f->off) = (float)v;
            }
            return 1;
        }
    }
    fprintf(stderr, "unknown material parameter '%.*s'; known:", (int)len, arg);
    for (size_t k = 0; k < sizeof(OVERRIDE_FIELDS) / sizeof(OVERRIDE_FIELDS[0]); k++) {
        fprintf(stderr, " %s", OVERRIDE_FIELDS[k].name);
    }
    fprintf(stderr, "\n");
    exit(2);
}

// Print the overrides that were applied (for the log of a run).
static void override_print(const material_t *def, const material_t *m)
{
    for (size_t k = 0; k < sizeof(OVERRIDE_FIELDS) / sizeof(OVERRIDE_FIELDS[0]); k++) {
        const override_field_t *f = &OVERRIDE_FIELDS[k];
        const char *a = (const char *)def + f->off, *b = (const char *)m + f->off;
        if (f->is_int && *(const int *)a != *(const int *)b) {
            printf("  override %s = %d (default %d)\n", f->name, *(const int *)b, *(const int *)a);
        } else if (!f->is_int && *(const float *)a != *(const float *)b) {
            printf("  override %s = %g (default %g)\n", f->name, (double)*(const float *)b, (double)*(const float *)a);
        }
    }
}
