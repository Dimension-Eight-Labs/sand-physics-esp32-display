// Soft power; see power.h.
#include "power.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "config.h"
#include "display.h"
#include "imu.h"
#include "led.h"

static const char *TAG = "power";

#define RTC_MAGIC 0x53414E44u // "SAND"

enum {
    STAGE_NONE,
    STAGE_ARMING, // first sleep: BOOT + arm timer
    STAGE_ARMED,  // second sleep: BOOT + motion
};

// RTC slow memory: kept through deep sleep, re-initialised from the image at
// every other boot (power-up, RESET, crash), so stale values never survive.
static RTC_DATA_ATTR struct {
    uint32_t magic;
    int mode;
    int stage;
    int off_reason;   // power_off_reason_t of the last power-off
    float off_mv;     // settled cell voltage then (< 0: no reading)
    int64_t off_us;   // RTC-clock time then (gettimeofday, kept through deep sleep)
} s_rtc;

static power_last_off_t s_last_off;

static int64_t rtc_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static bool s_resumed;
static bool s_arm_wake;
static const char *s_reason = "power-on / RESET";

void power_init(void)
{
    const esp_reset_reason_t rst = esp_reset_reason();
    const bool valid = s_rtc.magic == RTC_MAGIC;
    if (rst == ESP_RST_DEEPSLEEP && valid) {
        const uint32_t causes = esp_sleep_get_wakeup_causes();
        if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER) && s_rtc.stage == STAGE_ARMING) {
            s_arm_wake = true;
            s_reason = "motion-wake arming timer";
        } else if (causes & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
            const uint64_t pins = esp_sleep_get_ext1_wakeup_status();
            s_reason = pins & (1ULL << BOARD_BOOT_PIN) ? "BOOT button"
                       : pins & (1ULL << BOARD_IMU_INT2_PIN) ? "motion"
                                                              : "wake (ext1)";
            s_resumed = true;
        } else {
            s_reason = "wake (other)";
            s_resumed = true;
        }
    } else if (rst == ESP_RST_DEEPSLEEP) {
        s_reason = "wake (no saved state)";
    } else if (rst != ESP_RST_POWERON) {
        s_reason = rst == ESP_RST_SW         ? "software restart"
                   : rst == ESP_RST_PANIC    ? "restart after a crash"
                   : rst == ESP_RST_BROWNOUT ? "brown-out"
                   : rst == ESP_RST_USB      ? "USB reset"
                                             : "other reset";
    }
    if (s_resumed) {
        s_last_off = (power_last_off_t){
            .valid = true,
            .reason = (power_off_reason_t)s_rtc.off_reason,
            .mv_at_off = s_rtc.off_mv,
            .slept_s = (double)(rtc_now_us() - s_rtc.off_us) / 1e6,
        };
    }
    if (!s_arm_wake) {
        s_rtc.stage = STAGE_NONE;
    }
    if (rst == ESP_RST_DEEPSLEEP) {
        // The wake pins were switched to the RTC IO mux for the sleep, and
        // that (RTC-domain) setting survives the wake: hand them back to the
        // digital GPIO matrix, or BOOT could not be read.
        const gpio_num_t pins[] = { (gpio_num_t)BOARD_BOOT_PIN, (gpio_num_t)BOARD_IMU_INT2_PIN };
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
            rtc_gpio_hold_dis(pins[i]);
            rtc_gpio_deinit(pins[i]);
        }
    }
    // Pads held through the sleep stay held until each driver releases its
    // own pins; stop holding them automatically at the next sleep.
    gpio_deep_sleep_hold_dis();
}

bool power_is_arm_wake(void)
{
    return s_arm_wake;
}

bool power_resumed(void)
{
    return s_resumed;
}

const char *power_boot_reason(void)
{
    return s_reason;
}

int power_saved_mode(void)
{
    return s_resumed && s_rtc.magic == RTC_MAGIC ? s_rtc.mode : -1;
}

// Configure the wake sources for this stage and sleep.
static void sleep_now(bool motion)
{
    const gpio_num_t int2 = (gpio_num_t)BOARD_IMU_INT2_PIN;
    uint64_t mask = 1ULL << BOARD_BOOT_PIN; // external 10K pull-up, low while pressed
    if (motion) {
        // INT2 idles high and goes low on motion. The pull-up only matters if
        // the sensor leaves the line floating; it draws nothing while the
        // sensor drives it high.
        rtc_gpio_pullup_en(int2);
        rtc_gpio_pulldown_dis(int2);
        mask |= 1ULL << BOARD_IMU_INT2_PIN;
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (s_rtc.stage == STAGE_ARMING) {
        esp_sleep_enable_timer_wakeup((uint64_t)WOM_ARM_DELAY_S * 1000000);
    }
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
}

// Motion wake needs INT2 at its idle level; a pending event is cleared.
static bool int2_idle(void)
{
    const gpio_num_t int2 = (gpio_num_t)BOARD_IMU_INT2_PIN;
    gpio_set_direction(int2, GPIO_MODE_INPUT);
    gpio_set_pull_mode(int2, GPIO_PULLUP_ONLY);
    if (gpio_get_level(int2) == 0) {
        imu_clear_wake_on_motion();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return gpio_get_level(int2) == 1;
}

void power_arm_wake(void)
{
    // Re-assert the sleep levels (in case the pads were released at the wake).
    display_hold_pins();
    led_hold_pin();
    s_rtc.stage = STAGE_ARMED;
    bool motion = imu_rearm_wake_on_motion() == ESP_OK;
    if (motion && !int2_idle()) {
        // Moving right now: that is motion, wake fully.
        ESP_LOGI(TAG, "moving while arming motion wake");
    }
    sleep_now(motion);
}

power_last_off_t power_last_off(void)
{
    return s_last_off;
}

bool power_after_cutoff(void)
{
    return s_last_off.valid && s_last_off.reason == POWER_OFF_CUTOFF;
}

void power_off(int mode, bool imu_ok, power_off_reason_t reason)
{
    s_rtc.magic = RTC_MAGIC;
    s_rtc.mode = mode;
    s_rtc.off_reason = reason;

    led_sleep();
    display_sleep();
    bool motion = false;
    if (reason == POWER_OFF_CUTOFF) {
        // No motion wake on an empty cell: it would wake and drain it again.
        if (imu_ok) {
            imu_power_down();
        }
    }
#if WOM_ENABLE
    else if (imu_ok) {
        esp_err_t err = imu_wake_on_motion(WOM_THRESHOLD_MG, WOM_ODR, WOM_BLANKING);
        motion = err == ESP_OK;
        if (!motion) {
            ESP_LOGW(TAG, "wake-on-motion setup failed (%s): BOOT / RESET wake only", esp_err_to_name(err));
        }
    }
#endif
    s_rtc.stage = motion ? STAGE_ARMING : STAGE_NONE;

    // Drain log: the voltage with the backlight already off (the wake reading
    // is also taken before the backlight is used much), and the time.
    s_rtc.off_mv = battery_read_settled_mv();
    s_rtc.off_us = rtc_now_us();

    // BOOT is a wake source (low = pressed): wait until it is let go, or the
    // hold that powered off would wake the board again at once.
    int up = 0;
    while (up < 5) {
        up = gpio_get_level((gpio_num_t)BOARD_BOOT_PIN) ? up + 1 : 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    static const char *const WHY[] = { "BOOT held", "auto-off", "battery cutoff" };
    if (motion) {
        printf("power off (%s, battery %.3f V): wakes on BOOT or RESET, and on motion after %d s\n", WHY[reason],
               (double)(s_rtc.off_mv / 1000.0f), WOM_ARM_DELAY_S);
    } else {
        printf("power off (%s, battery %.3f V): wakes on BOOT or RESET\n", WHY[reason],
               (double)(s_rtc.off_mv / 1000.0f));
    }
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(20)); // let the console drain
    sleep_now(false);
}
