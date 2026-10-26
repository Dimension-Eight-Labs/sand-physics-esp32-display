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
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "imu";

#define REG_WHO_AM_I 0x00
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03 // accel full scale (bits 6:4) + ODR (bits 3:0)
#define REG_CTRL5    0x06 // on-chip low-pass filters
#define REG_CTRL7    0x08 // sensor enable
#define REG_AX_L     0x35

#define WHO_AM_I_VAL 0x05
#define ACC_FS_8G    0x02 // CTRL2 aFS
#define ACC_ODR_250  0x05 // CTRL2 aODR (demo enum acc_odr_norm_250)
#define ACC_LSB_G    (8.0f / 32768.0f)

#define IMU_TASK_STACK 3072
#define IMU_TASK_PRIO  5
#define IMU_TASK_CORE  0

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

static void imu_task(void *arg)
{
    (void)arg;
    float lp[3] = { 0.0f, 1.0f, 0.0f }; // gravity, screen frame; start "down"
    bool primed = false;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / IMU_POLL_HZ); // needs CONFIG_FREERTOS_HZ=1000
    int fails = 0;

    for (;;) {
        xTaskDelayUntil(&last, period);

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

esp_err_t imu_init(void)
{
    // Until the sensor delivers, report plain "down" so the sand still falls.
    s_state.gy = 1.0f;

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

    ESP_LOGI(TAG, "QMI8658 at 0x%02X, WHO_AM_I 0x%02X, +/-8 g", addr, who);

    BaseType_t ok = xTaskCreatePinnedToCore(imu_task, "imu", IMU_TASK_STACK, NULL, IMU_TASK_PRIO, NULL,
                                            IMU_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
