// QMI8658 accelerometer: axis mapping, low-pass gravity and shake detection.
// Runs its own polling task.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    float raw[3];        // last accelerometer reading, chip axes, in g
    float gx, gy, gz;    // low-passed gravity in the screen frame, g (x right, y down, z out of glass)
    float shake;         // largest shake excess (g above threshold) since last consume, 0 = none
    float shake_dx;      // apparent push direction on the sand for that shake (screen frame)
    float shake_dy;
    bool ok;             // sensor found and delivering data
} imu_state_t;

typedef struct {
    bool found;          // answered on I2C
    int addr;            // 7-bit address it answered on, -1 if not found
    int who_am_i;        // WHO_AM_I register value, -1 if not read
} imu_info_t;

// Probe the sensor, configure it and start the polling task.
esp_err_t imu_init(void);

// Result of the probe done by imu_init().
imu_info_t imu_get_info(void);

// Copy the latest state. With consume = true the shake fields are reset.
void imu_get(imu_state_t *out, bool consume);
