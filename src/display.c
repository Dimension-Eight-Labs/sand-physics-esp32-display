// ST7789 display driver glue: esp_lcd SPI panel IO + built-in ST7789 panel
// driver, Waveshare's vendor register sequence, LEDC backlight.
#include "display.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"

static const char *TAG = "display";

// Largest single SPI transaction; esp_lcd splits a frame into chunks of this
// size and queues them all, so the queue must hold a whole frame.
#define CHUNK_BYTES   (SCREEN_W * 64 * sizeof(uint16_t))
#define FRAME_BYTES   (SCREEN_W * SCREEN_H * sizeof(uint16_t))
#define QUEUE_DEPTH   ((FRAME_BYTES + CHUNK_BYTES - 1) / CHUNK_BYTES + 2)

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES     LEDC_TIMER_13_BIT
#define BL_LEDC_MAX     ((1 << 13) - 1)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static SemaphoreHandle_t s_done;
static bool s_busy;

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[14];
} lcd_init_cmd_t;

// Vendor register settings from Waveshare's Arduino demo
// (Arduino/examples/LCD_Image/Display_ST7789.cpp, LCD_Init). Sleep-out,
// MADCTL and COLMOD are sent by esp_lcd's ST7789 driver; inversion, display-on
// and mirroring go through the esp_lcd API so its cached state stays correct.
static const lcd_init_cmd_t VENDOR_INIT[] = {
    { 0xB0, 2, { 0x00, 0xE8 } },                   // RAMCTRL
    { 0xB2, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 } }, // porch setting
    { 0xB7, 1, { 0x35 } },                         // gate control
    { 0xBB, 1, { 0x35 } },                         // VCOM
    { 0xC0, 1, { 0x2C } },                         // LCM control
    { 0xC2, 1, { 0x01 } },                         // VDV/VRH command enable
    { 0xC3, 1, { 0x13 } },                         // VRH set
    { 0xC4, 1, { 0x20 } },                         // VDV set
    { 0xC6, 1, { 0x0F } },                         // frame rate control (60 Hz)
    { 0xD0, 2, { 0xA4, 0xA1 } },                   // power control 1
    { 0xD6, 1, { 0xA1 } },
    { 0xE0, 14, { 0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30 } }, // +gamma
    { 0xE1, 14, { 0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32 } }, // -gamma
};

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz = BOARD_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t ch = {
        .gpio_num = BOARD_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

void display_set_backlight(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, (uint32_t)(BL_LEDC_MAX * percent / 100));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

esp_err_t display_init(void)
{
    // Backlight off until the first frame is on the panel.
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "semaphore");

    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .spi_mode = BOARD_LCD_SPI_MODE,
        .pclk_hz = BOARD_LCD_SPI_HZ,
        .trans_queue_depth = QUEUE_DEPTH,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = BOARD_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    for (size_t i = 0; i < sizeof(VENDOR_INIT) / sizeof(VENDOR_INIT[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, VENDOR_INIT[i].cmd, VENDOR_INIT[i].data,
                                                      VENDOR_INIT[i].len), TAG, "vendor init");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, BOARD_LCD_INVERT), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y), TAG, "gap");

    s_fb = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "frame buffer (%u bytes)", (unsigned)FRAME_BYTES);

    // Clear panel RAM (including any garbage from before reset), then light up.
    display_push_async();
    display_wait();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    ESP_LOGI(TAG, "ST7789 %dx%d @ %d MHz, offset (%d,%d), %u-byte chunks x %u",
             SCREEN_W, SCREEN_H, BOARD_LCD_SPI_HZ / 1000000, BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y,
             (unsigned)CHUNK_BYTES, (unsigned)QUEUE_DEPTH);
    return ESP_OK;
}

uint16_t *display_framebuffer(void)
{
    return s_fb;
}

void display_push_async(void)
{
    s_busy = true;
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SCREEN_W, SCREEN_H, s_fb);
}

void display_wait(void)
{
    if (!s_busy) {
        return;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "frame push timed out");
    }
    s_busy = false;
}
