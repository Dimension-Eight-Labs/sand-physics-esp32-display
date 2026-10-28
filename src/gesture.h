// BOOT button gestures: click, double click, long press, hold to power off.
// Hardware-free (debouncing included), so tools/sim_power.c can check that the
// gestures cannot be confused. Timings and rules: config.h, "Button (BOOT)".
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GESTURE_CLICK     (1u << 0)
#define GESTURE_DOUBLE    (1u << 1)
#define GESTURE_LONG      (1u << 2)
#define GESTURE_POWER_OFF (1u << 3)

#define GESTURE_POLL_MS 10 // gesture_update() must be called at this interval

typedef struct {
    bool raw_last;      // last raw sample
    int same;           // polls the raw level has been stable
    bool down;          // debounced state
    bool ignore;        // press in progress is not a gesture (down at start, or power-off fired)
    bool second;        // press in progress started within the double-click window
    bool pending;       // a click waits for the double-click window to pass
    uint32_t t_down;    // debounced press time
    uint32_t t_up;      // debounced release time of the pending click
} gesture_t;

// pressed_now: the raw button level at start. If it is down, that press is
// ignored until it is released.
void gesture_init(gesture_t *g, bool pressed_now, uint32_t now_ms);

// Feed one raw sample (true = pressed) every GESTURE_POLL_MS. Returns the
// GESTURE_* events completed by this sample.
uint32_t gesture_update(gesture_t *g, bool pressed, uint32_t now_ms);

// Hold-to-power-off progress: < 0 when no indicator should be shown, else
// 0..1 from BTN_HOLD_SHOW_MS to BTN_POWER_OFF_MS.
float gesture_hold_progress(const gesture_t *g, uint32_t now_ms);
