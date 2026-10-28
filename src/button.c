// BOOT button polled from an esp_timer every GESTURE_POLL_MS.
#include "button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "config.h"

static const char *TAG = "button";

static esp_timer_handle_t s_timer;
static gesture_t s_gesture;          // timer callback only
static uint32_t s_events;            // accessed with __atomic builtins
static uint32_t s_progress_permille; // hold progress + 1 (0 = none), __atomic

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool button_is_down(void)
{
    return gpio_get_level(BOARD_BOOT_PIN) == 0; // active low
}

static void poll_cb(void *arg)
{
    (void)arg;
    const uint32_t t = now_ms();
    const uint32_t ev = gesture_update(&s_gesture, button_is_down(), t);
    if (ev) {
        __atomic_fetch_or(&s_events, ev, __ATOMIC_RELAXED);
    }
    const float p = gesture_hold_progress(&s_gesture, t);
    __atomic_store_n(&s_progress_permille, p < 0.0f ? 0u : 1u + (uint32_t)(p * 1000.0f), __ATOMIC_RELAXED);
}

esp_err_t button_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    // A press already down (the one that woke the board) is not a gesture.
    gesture_init(&s_gesture, button_is_down(), now_ms());

    const esp_timer_create_args_t args = {
        .callback = poll_cb,
        .name = "button",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG, "timer");
    return esp_timer_start_periodic(s_timer, GESTURE_POLL_MS * 1000);
}

uint32_t button_take_events(void)
{
    return __atomic_exchange_n(&s_events, 0, __ATOMIC_RELAXED);
}

float button_hold_progress(void)
{
    const uint32_t v = __atomic_load_n(&s_progress_permille, __ATOMIC_RELAXED);
    return v == 0 ? -1.0f : (float)(v - 1) / 1000.0f;
}
