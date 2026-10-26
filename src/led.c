// WS2812B driven through the espressif/led_strip RMT backend.
#include "led.h"

#include <math.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

#include "config.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static float s_activity;      // smoothed
static uint32_t s_last_rgb = 0xFFFFFFFF;

esp_err_t led_init(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_RGB_PIN,
        .max_leds = BOARD_RGB_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        // At least stop the data line from floating and latching noise.
        s_strip = NULL;
        gpio_set_direction(BOARD_RGB_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(BOARD_RGB_PIN, 0);
        ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(err));
        return err;
    }
    // Defined state: off, until the running mode sets its colour.
    return led_strip_clear(s_strip);
}

void led_update(uint32_t color, float activity, uint32_t now_ms)
{
    if (!s_strip) {
        return;
    }
    if (activity < 0.0f) {
        activity = 0.0f;
    } else if (activity > 1.0f) {
        activity = 1.0f;
    }
    // Fast attack, slow release so splashes flash and then fade.
    float k = activity > s_activity ? 0.5f : 0.03f;
    s_activity += k * (activity - s_activity);

    float phase = (float)(now_ms % LED_PULSE_PERIOD_MS) / (float)LED_PULSE_PERIOD_MS;
    float pulse = 0.65f + 0.35f * sinf(phase * 2.0f * 3.14159265f);
    float level = ((float)LED_MIN_LEVEL + (float)(LED_MAX_LEVEL - LED_MIN_LEVEL) * s_activity) * pulse / 255.0f;

    uint32_t r = (uint32_t)((float)((color >> 16) & 0xFF) * level);
    uint32_t g = (uint32_t)((float)((color >> 8) & 0xFF) * level);
    uint32_t b = (uint32_t)((float)(color & 0xFF) * level);
    uint32_t rgb = (r << 16) | (g << 8) | b;
    if (rgb == s_last_rgb) {
        return;
    }
    s_last_rgb = rgb;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}
