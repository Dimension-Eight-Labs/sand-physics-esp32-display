// QMI8658 driver (accelerometer only is used).
//
// Register usage mirrors Waveshare's ESP-IDF demo (main/QMI8658/QMI8658.c):
// CTRL1 auto-increment, CTRL7 = 0x43 to run the sensors, little-endian data
// from AX_L. Range and rate are chosen for this demo.
#include "imu.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "imu";

#define REG_WHO_AM_I 0x00
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03 // accel full scale (bits 6:4) + ODR (bits 3:0)
#define REG_CTRL5    0x06 // on-chip low-pass filters
#define REG_CTRL7    0x08 // sensor enable
#define REG_CTRL9    0x0A // host commands
#define REG_CAL1_L   0x0B
#define REG_CAL1_H   0x0C
#define REG_STATUSINT 0x2D
#define REG_STATUS1  0x2F
#define REG_AX_L     0x35
#define REG_RESET    0x60

#define CTRL1_INT2_EN   0x10 // INT2 output enabled (later datasheet revisions; reserved in rev 0.6)
#define CMD_WRITE_WOM   0x08 // CTRL9: apply the CAL1 wake-on-motion settings
#define CMD_ACK         0x00
#define ACC_FS_4G       0x01

#define WHO_AM_I_VAL 0x05
#define ACC_FS_8G    0x02 // CTRL2 aFS
#define ACC_ODR_250  0x05 // CTRL2 aODR (demo enum acc_odr_norm_250)
#define ACC_LSB_G    (8.0f / 32768.0f)

#define IMU_TASK_STACK 3072
#define IMU_TASK_PRIO  5
// Core 1 (with rendering): the simulation on core 0 has the tight per-step
// budget, rendering uses a small part of each frame.
#define IMU_TASK_CORE  1

static SemaphoreHandle_t s_ready; // imu_task -> imu_init: setup finished; -> imu_stop: stopped
static bool s_stop_req;           // accessed with __atomic builtins
static esp_err_t s_init_err = ESP_FAIL;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static imu_state_t s_state;
static imu_info_t s_info = { .found = false, .addr = -1, .who_am_i = -1 };

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 50);
}

static inline float pick(const float a[3], int src, int sign)
{
    return (float)sign * a[src];
}

static esp_err_t imu_setup(void);

static void imu_task(void *arg)
{
    (void)arg;
    s_init_err = imu_setup();
    xSemaphoreGive(s_ready);
    if (s_init_err != ESP_OK) {
        vTaskDelete(NULL);
    }

    float lp[3] = { 0.0f, 1.0f, 0.0f }; // gravity, screen frame; start "down"
    bool primed = false;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / IMU_POLL_HZ); // needs CONFIG_FREERTOS_HZ=1000
    int fails = 0;

    for (;;) {
        xTaskDelayUntil(&last, period);
        if (__atomic_load_n(&s_stop_req, __ATOMIC_ACQUIRE)) {
            xSemaphoreGive(s_ready);
            vTaskDelete(NULL);
        }

        uint8_t b[6];
        if (reg_read(REG_AX_L, b, sizeof(b)) != ESP_OK) {
            if (++fails == 50) {
                ESP_LOGW(TAG, "repeated read failures");
            }
            continue;
        }
        fails = 0;

        float a[3];
        for (int i = 0; i < 3; i++) {
            a[i] = (float)(int16_t)((b[2 * i + 1] << 8) | b[2 * i]) * ACC_LSB_G;
        }

        // Screen-frame acceleration reading, then gravity = -reading
        // (an accelerometer at rest reads +1 g pointing up).
        float g[3] = {
            -pick(a, IMU_SCREEN_X_SRC, IMU_SCREEN_X_SIGN),
            -pick(a, IMU_SCREEN_Y_SRC, IMU_SCREEN_Y_SIGN),
            -pick(a, IMU_SCREEN_Z_SRC, IMU_SCREEN_Z_SIGN),
        };
        if (!primed) {
            memcpy(lp, g, sizeof(lp));
            primed = true;
        }
        for (int i = 0; i < 3; i++) {
            lp[i] += IMU_LPF_ALPHA * (g[i] - lp[i]);
        }

        // Shake: total magnitude far from 1 g. The sand is pushed along the
        // fast-changing part of the apparent gravity.
        float mag = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        float excess = fabsf(mag - 1.0f) - SHAKE_THRESHOLD_G;

        portENTER_CRITICAL(&s_lock);
        memcpy(s_state.raw, a, sizeof(a));
        s_state.gx = lp[0];
        s_state.gy = lp[1];
        s_state.gz = lp[2];
        if (excess > s_state.shake) {
            s_state.shake = excess;
            s_state.shake_dx = g[0] - lp[0];
            s_state.shake_dy = g[1] - lp[1];
        }
        s_state.ok = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

imu_info_t imu_get_info(void)
{
    return s_info;
}

void imu_get(imu_state_t *out, bool consume)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    if (consume) {
        s_state.shake = 0.0f;
        s_state.shake_dx = 0.0f;
        s_state.shake_dy = 0.0f;
    }
    portEXIT_CRITICAL(&s_lock);
}

// CTRL9 command handshake. The datasheet revisions differ: rev 0.6 sets
// STATUS1 bit 0 when done and clears it when STATUS1 is read; later ones set
// STATUSINT bit 7 and want CMD_ACK written to CTRL9. Both are handled.
static esp_err_t ctrl9(uint8_t cmd)
{
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL9, cmd), TAG, "CTRL9");
    esp_err_t done = ESP_ERR_TIMEOUT;
    for (int i = 0; i < 100 && done != ESP_OK; i++) {
        uint8_t si = 0, s1 = 0;
        if (reg_read(REG_STATUSINT, &si, 1) == ESP_OK && reg_read(REG_STATUS1, &s1, 1) == ESP_OK &&
            ((si & 0x80) || (s1 & 0x01))) {
            done = ESP_OK;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    reg_write(REG_CTRL9, CMD_ACK);
    vTaskDelay(pdMS_TO_TICKS(1));
    uint8_t s1;
    reg_read(REG_STATUS1, &s1, 1); // clears CmdDone (rev 0.6)
    return done;
}

// Leave wake-on-motion (the sensor stays powered through deep sleep and may
// still be in it), the datasheet's exit sequence, then a soft reset.
static void leave_wake_on_motion(void)
{
    reg_write(REG_CTRL7, 0x00);
    reg_write(REG_CAL1_L, 0x00);
    reg_write(REG_CAL1_H, 0x00);
    if (ctrl9(CMD_WRITE_WOM) != ESP_OK) {
        ESP_LOGW(TAG, "no CTRL9 acknowledge leaving wake-on-motion");
    }
    reg_write(REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// I2C bus, probe (main address, then the alternative) and device handle.
static esp_err_t open_device(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = BOARD_I2C_PIN_SDA,
        .scl_io_num = BOARD_I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    uint16_t addr = BOARD_IMU_ADDR;
    if (i2c_master_probe(s_bus, addr, 50) != ESP_OK) {
        addr = BOARD_IMU_ADDR_ALT;
        ESP_RETURN_ON_ERROR(i2c_master_probe(s_bus, addr, 50), TAG,
                            "QMI8658 not found at 0x%02X or 0x%02X", BOARD_IMU_ADDR, BOARD_IMU_ADDR_ALT);
        ESP_LOGW(TAG, "QMI8658 answered at fallback address 0x%02X", addr);
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "i2c device");
    s_info.found = true;
    s_info.addr = addr;

    return ESP_OK;
}

// Bus, probe and sensor configuration. Runs in the IMU task, so the I2C
// interrupt is installed on that task's core (see IMU_TASK_CORE).
static esp_err_t imu_setup(void)
{
    ESP_RETURN_ON_ERROR(open_device(), TAG, "device");

    leave_wake_on_motion();
    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_WHO_AM_I, &who, 1), TAG, "WHO_AM_I");
    s_info.who_am_i = who;
    if (who != WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "unexpected WHO_AM_I 0x%02X (expected 0x%02X), continuing", who, WHO_AM_I_VAL);
    }

    // As in the Waveshare demo: keep the oscillator on (bit 0 = 0) and enable
    // register auto-increment (bit 6) for burst reads.
    uint8_t ctrl1 = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CTRL1, &ctrl1, 1), TAG, "CTRL1");
    ctrl1 = (uint8_t)((ctrl1 & 0xFE) | 0x40);
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL1, ctrl1), TAG, "CTRL1");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL2, (ACC_FS_8G << 4) | ACC_ODR_250), TAG, "CTRL2");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL5, 0x00), TAG, "CTRL5"); // filtering is done in software
    // Same enable value as the demo (accel + gyro + high-speed clock); the
    // gyro is unused but this is the configuration known to work on this board.
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL7, 0x43), TAG, "CTRL7");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "QMI8658 at 0x%02X, WHO_AM_I 0x%02X, +/-8 g", s_info.addr, who);
    return ESP_OK;
}

esp_err_t imu_init(void)
{
    // Until the sensor delivers, report plain "down" so the sand still falls.
    s_state.gy = 1.0f;

    s_ready = xSemaphoreCreateBinary();
    if (!s_ready) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(imu_task, "imu", IMU_TASK_STACK, NULL, IMU_TASK_PRIO, NULL,
                                            IMU_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_ready, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_init_err;
}

static void stop_task(void)
{
    if (s_init_err != ESP_OK || __atomic_load_n(&s_stop_req, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&s_stop_req, true, __ATOMIC_RELEASE);
    xSemaphoreTake(s_ready, pdMS_TO_TICKS(100));
}

esp_err_t imu_wake_on_motion(uint8_t threshold_mg, uint8_t odr, uint8_t blanking)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    stop_task();
    // Datasheet section 9.4: sensors off, accelerometer rate and range, WoM
    // threshold and interrupt (INT2, idle high, so the event drives it low),
    // CTRL9 command, accelerometer on (alone, so low-power rates apply).
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL7, 0x00), TAG, "CTRL7");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL2, (ACC_FS_4G << 4) | (odr & 0x0F)), TAG, "CTRL2");
    ESP_RETURN_ON_ERROR(reg_write(REG_CAL1_L, threshold_mg), TAG, "CAL1_L");
    ESP_RETURN_ON_ERROR(reg_write(REG_CAL1_H, (uint8_t)(0xC0 | (blanking & 0x3F))), TAG, "CAL1_H");
    ESP_RETURN_ON_ERROR(ctrl9(CMD_WRITE_WOM), TAG, "WoM command");
    uint8_t ctrl1 = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CTRL1, &ctrl1, 1), TAG, "CTRL1");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL1, (uint8_t)((ctrl1 & 0xFE) | CTRL1_INT2_EN)), TAG, "CTRL1");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL7, 0x01), TAG, "CTRL7");
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t s1;
    return reg_read(REG_STATUS1, &s1, 1); // INT2 back to its idle level
}

esp_err_t imu_clear_wake_on_motion(void)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t s1;
    return reg_read(REG_STATUS1, &s1, 1);
}

esp_err_t imu_rearm_wake_on_motion(void)
{
    // A brief wake (no task, no reconfiguration): the sensor is still in
    // wake-on-motion from before; only the interrupt line is reset.
    ESP_RETURN_ON_ERROR(open_device(), TAG, "device");
    return imu_clear_wake_on_motion();
}

esp_err_t imu_power_down(void)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    stop_task();
    // Datasheet "Power-Down": sensors off, internal oscillator off (about
    // 20 uA). Register values are kept; the next imu_init() resets it.
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL7, 0x00), TAG, "CTRL7");
    uint8_t ctrl1 = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CTRL1, &ctrl1, 1), TAG, "CTRL1");
    return reg_write(REG_CTRL1, (uint8_t)(ctrl1 | 0x01));
}
