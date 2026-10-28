// Soft power: deep sleep, wake sources, mode memory across sleep.
//
// Power off has two sleeps. The first wakes only on BOOT (or RESET), plus a
// timer after WOM_ARM_DELAY_S; that timer wake is brief (power_arm_wake():
// no display, no LED) and goes back to sleep with motion wake armed too.
#pragma once

#include <stdbool.h>

// Call first in app_main: reads why the chip started.
void power_init(void);

// True if this boot is the brief timer wake that arms motion wake; then call
// power_arm_wake() before bringing anything else up.
bool power_is_arm_wake(void);

// Arm motion wake and sleep again. Does not return.
void power_arm_wake(void);

// Why the chip started, for the banner ("power-on / RESET", "BOOT button",
// "motion", ...).
const char *power_boot_reason(void);

// True when this boot is a wake from power-off (deep sleep).
bool power_resumed(void);

// The sand mode saved at power-off, or -1 if there is none (cold start,
// RESET, or not a wake from sleep).
int power_saved_mode(void);

typedef enum {
    POWER_OFF_BUTTON, // BOOT held
    POWER_OFF_AUTO,   // auto-off
    POWER_OFF_CUTOFF, // battery below the cutoff: no motion wake
} power_off_reason_t;

// What the last power-off recorded (drain log), valid after a wake from it.
typedef struct {
    bool valid;
    power_off_reason_t reason;
    float mv_at_off;   // settled cell voltage at power-off, mV (< 0 if none)
    double slept_s;    // from power-off to this boot, RTC clock
} power_last_off_t;

// Power off: saves `mode`, puts the LED, display and IMU to sleep, records
// the settled battery voltage and the time, waits for BOOT to be released
// and enters deep sleep. The caller has played the power-off animation.
// Motion wake is armed unless `reason` is POWER_OFF_CUTOFF. Does not return.
void power_off(int mode, bool imu_ok, power_off_reason_t reason);

// The record of the power-off this boot woke from.
power_last_off_t power_last_off(void);

// True if the last power-off was a battery cutoff (this boot woke from it).
bool power_after_cutoff(void);
