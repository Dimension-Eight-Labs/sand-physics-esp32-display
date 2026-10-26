// BOOT button polled from an esp_timer every 10 ms.
#include "button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "config.h"

static const char *TAG = "button";

#define POLL_MS        10
#define DEBOUNCE_POLLS 3   // level must be stable this many polls
#define MIN_PRESS_MS   40

static esp_timer_handle_t s_timer;
static uint32_t s_events; // accessed with __atomic builtins

static void poll_cb(void *arg)
{
    static bool stable_down, last_raw;
    static int same_count;
    static int held_ms;
    static bool long_fired;
    (void)arg;

    bool raw = gpio_get_level(BOARD_BOOT_PIN) == 0; // active low
    if (raw == last_raw) {
        if (same_count < DEBOUNCE_POLLS) {
            same_count++;
        }
    } else {
        same_count = 0;
        last_raw = raw;
    }
    if (same_count >= DEBOUNCE_POLLS && raw != stable_down) {
        stable_down = raw;
        if (stable_down) {
            held_ms = 0;
            long_fired = false;
        } else if (!long_fired && held_ms >= MIN_PRESS_MS) {
            __atomic_fetch_or(&s_events, BUTTON_EVT_SHORT, __ATOMIC_RELAXED);
        }
    }
    if (stable_down) {
        held_ms += POLL_MS;
        if (!long_fired && held_ms >= BTN_LONG_MS) {
            long_fired = true;
            __atomic_fetch_or(&s_events, BUTTON_EVT_LONG, __ATOMIC_RELAXED);
        }
    }
}

esp_err_t button_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");

    const esp_timer_create_args_t args = {
        .callback = poll_cb,
        .name = "button",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG, "timer");
    return esp_timer_start_periodic(s_timer, POLL_MS * 1000);
}

uint32_t button_take_events(void)
{
    return __atomic_exchange_n(&s_events, 0, __ATOMIC_RELAXED);
}
