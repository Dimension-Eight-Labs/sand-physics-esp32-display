// QMI8658 accelerometer: axis mapping, low-pass gravity and shake detection.
// Runs its own polling task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// For deep sleep: stop the polling task and put the sensor into
// wake-on-motion (accelerometer only, low-power `odr` = CTRL2 aODR 0x0C..0x0F,
// `blanking` samples ignored at the start). INT2 idles high and goes low on
// motion. The next imu_init() takes it out of this mode again.
esp_err_t imu_wake_on_motion(uint8_t threshold_mg, uint8_t odr, uint8_t blanking);

// Reset INT2 to its idle level (reads STATUS1) after a motion event.
esp_err_t imu_clear_wake_on_motion(void);

// On a brief wake from deep sleep with the sensor still in wake-on-motion
// (imu_init() not called): open the bus and reset INT2 to idle.
esp_err_t imu_rearm_wake_on_motion(void);

// For deep sleep without motion wake (battery cutoff): stop the polling task
// and put the sensor into power-down.
esp_err_t imu_power_down(void);
