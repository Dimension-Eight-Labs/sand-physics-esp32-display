// WS2812B driven through the espressif/led_strip RMT backend.
#include "led.h"

#include <math.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "led_strip.h"

#include "config.h"
#include "ledcolor.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static float s_activity;      // smoothed
static uint32_t s_last_rgb = 0xFFFFFFFF;

esp_err_t led_init(void)
{
    // Released from the deep-sleep hold (if any) before RMT takes the pin.
    gpio_hold_dis(BOARD_RGB_PIN);
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_RGB_PIN,
        .max_leds = BOARD_RGB_COUNT,
        .led_model = LED_MODEL_WS2812,
        // The one place channel order is handled (board.h); led_strip puts
        // each channel at its position, everything else is plain RGB.
        .color_component_format = BOARD_RGB_ORDER_RGB ? LED_STRIP_COLOR_COMPONENT_FMT_RGB
                                                      : LED_STRIP_COLOR_COMPONENT_FMT_GRB,
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

static void send(uint32_t rgb)
{
    if (!s_strip || rgb == s_last_rgb) {
        return;
    }
    s_last_rgb = rgb;
    led_strip_set_pixel(s_strip, 0, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    led_strip_refresh(s_strip);
}

uint32_t led_linear(uint32_t color, int level)
{
    return ledcolor_drive(color, (float)level);
}

void led_show(uint32_t color, int level)
{
    send(ledcolor_drive(color, (float)level));
}

void led_update(uint32_t color, float activity, float scale, uint32_t now_ms)
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
    float level = ((float)LED_MIN_LEVEL + (float)(LED_MAX_LEVEL - LED_MIN_LEVEL) * s_activity) * pulse;
    // Brightness only: the colour is linearised once in ledcolor_drive().
    send(ledcolor_drive(color, level * (scale < 0.0f ? 0.0f : scale)));
}

void led_sleep(void)
{
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    // Keep the line low long enough for the LED to latch "off" (reset > 280 us).
    gpio_reset_pin(BOARD_RGB_PIN);
    gpio_set_direction(BOARD_RGB_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_RGB_PIN, 0);
    esp_rom_delay_us(500);
    led_hold_pin();
}

void led_hold_pin(void)
{
    // Hold the data line at the sleep level. High costs nothing against the
    // board's 10 kOhm pull-up, and the LED latches nothing while it stays
    // high (a WS2812 latches only at the end of a low reset pulse).
    gpio_set_direction(BOARD_RGB_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_RGB_PIN, SLEEP_LED_DATA_LOW ? 0 : 1);
    gpio_hold_en(BOARD_RGB_PIN);
}
