// BOOT button gestures; see gesture.h and config.h.
#include "gesture.h"

#include "config.h"

#define DEBOUNCE_POLLS 3  // raw level must be stable this many polls
#define MIN_PRESS_MS   40 // shorter presses are noise

void gesture_init(gesture_t *g, bool pressed_now, uint32_t now_ms)
{
    *g = (gesture_t){
        .raw_last = pressed_now,
        .same = DEBOUNCE_POLLS,
        .down = pressed_now,
        .ignore = pressed_now,
        .t_down = now_ms,
    };
}

uint32_t gesture_update(gesture_t *g, bool pressed, uint32_t now_ms)
{
    uint32_t ev = 0;
    if (pressed == g->raw_last) {
        if (g->same < DEBOUNCE_POLLS) {
            g->same++;
        }
    } else {
        g->same = 0;
        g->raw_last = pressed;
    }

    if (g->same >= DEBOUNCE_POLLS && pressed != g->down) {
        g->down = pressed;
        if (pressed) {
            // A press inside the window makes this the second of a double;
            // the pending click is then never reported on its own. After
            // the window, the click stands (its timeout may not have been
            // checked yet in this same poll).
            g->second = g->pending && now_ms - g->t_up <= BTN_DOUBLE_MS;
            if (g->pending && !g->second) {
                ev |= GESTURE_CLICK;
            }
            g->pending = false;
            g->t_down = now_ms;
        } else if (g->ignore) {
            g->ignore = false;
        } else {
            const uint32_t held = now_ms - g->t_down;
            if (held < MIN_PRESS_MS) {
                g->pending = g->second; // noise: a waiting first click still counts
            } else if (held < BTN_LONG_MS) {
                if (g->second) {
                    ev |= GESTURE_DOUBLE;
                } else {
                    g->pending = true;
                    g->t_up = now_ms;
                }
            } else if (held < BTN_HOLD_SHOW_MS) {
                ev |= GESTURE_LONG;
            }
            // Released while the power-off bar showed: cancelled, nothing.
            g->second = false;
        }
    }

    if (g->down && !g->ignore && now_ms - g->t_down >= BTN_POWER_OFF_MS) {
        ev |= GESTURE_POWER_OFF;
        g->ignore = true; // the rest of this press does nothing
    }
    if (g->pending && !g->down && now_ms - g->t_up > BTN_DOUBLE_MS) {
        g->pending = false;
        ev |= GESTURE_CLICK;
    }
    return ev;
}

float gesture_hold_progress(const gesture_t *g, uint32_t now_ms)
{
    if (!g->down || g->ignore) {
        return -1.0f;
    }
    const uint32_t held = now_ms - g->t_down;
    if (held < BTN_HOLD_SHOW_MS) {
        return -1.0f;
    }
    const float p = (float)(held - BTN_HOLD_SHOW_MS) / (float)(BTN_POWER_OFF_MS - BTN_HOLD_SHOW_MS);
    return p > 1.0f ? 1.0f : p;
}
