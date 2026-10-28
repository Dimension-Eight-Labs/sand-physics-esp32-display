// Battery voltage reading; see battery.h.
#include "battery.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "battery";

#define BAT_TASK_STACK 3072
#define BAT_TASK_PRIO  1 // below everything else
#define BAT_TASK_CORE  1 // never on the simulation's core 0
// 6 dB: input range up to ~1.75 V; BAT_ADC is at most 4.2 V / 3 = 1.4 V, so
// this is the most resolving range that covers it.
#define BAT_ATTEN      ADC_ATTEN_DB_6

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static batt_t s_state; // under s_lock
static batt_t s_model; // task only
static SemaphoreHandle_t s_adc_mutex; // the sampling task vs. settled readings
static float s_boot_mv = -1.0f;

// Average of `samples` conversions, as the cell voltage in mV (< 0 on
// error). About 60 us per conversion.
static float read_cell_mv(int samples)
{
    int sum = 0, n = 0;
    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    for (int i = 0; i < samples; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) {
            continue;
        }
        if (s_cali) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
                continue;
            }
        } else {
            mv = raw * 1750 / 4095; // rough, uncalibrated
        }
        sum += mv;
        n++;
    }
    xSemaphoreGive(s_adc_mutex);
    return n ? (float)sum / (float)n * BAT_DIVIDER * BAT_ADC_SCALE : -1.0f;
}

static void publish(void)
{
    portENTER_CRITICAL(&s_lock);
    s_state = s_model;
    portEXIT_CRITICAL(&s_lock);
}

static void battery_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        xTaskDelayUntil(&last, pdMS_TO_TICKS(BAT_SAMPLE_MS));
        const float mv = read_cell_mv(BAT_ADC_SAMPLES);
        if (mv > 0.0f) {
            batt_update(&s_model, mv, (float)BAT_SAMPLE_MS / 1000.0f);
            publish();
        }
    }
}

esp_err_t battery_init(void)
{
    batt_init(&s_model);
    s_adc_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_adc_mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BOARD_BAT_ADC_PIN, &unit, &s_chan), TAG, "pin");
    const adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "unit");
    const adc_oneshot_chan_cfg_t chan_cfg = { .atten = BAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg), TAG, "channel");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit, .chan = s_chan, .atten = BAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration in eFuse: battery readings are rough");
    }
    // First reading now, settled (boot checks and the drain log use it), so
    // the banner and an early double click have one too.
    const float mv = battery_read_settled_mv();
    s_boot_mv = mv;
    if (mv > 0.0f) {
        batt_update(&s_model, mv, 0.0f);
        publish();
    }
    if (xTaskCreatePinnedToCore(battery_task, "battery", BAT_TASK_STACK, NULL, BAT_TASK_PRIO, NULL, BAT_TASK_CORE) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void battery_get(batt_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
}

const char *battery_calibration(void)
{
    return s_cali ? "curve fitting" : "none (uncalibrated)";
}

float battery_read_settled_mv(void)
{
    if (!s_adc) {
        return -1.0f;
    }
    // The divider is always connected and C8 is charged, but the first
    // conversions after the ADC (re)starts can be off: discard some, wait.
    read_cell_mv(8);
    vTaskDelay(pdMS_TO_TICKS(BAT_SETTLE_MS));
    return read_cell_mv(BAT_SETTLED_SAMPLES);
}

float battery_boot_mv(void)
{
    return s_boot_mv;
}
