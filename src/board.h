// Board definition: Waveshare ESP32-S3-LCD-1.47B
//
// Every value below is taken from Waveshare's own material. Abbreviations:
//   [WIKI]     https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B (pin tables)
//   [IDF-DEMO] ESP32-S3-LCD-1.47B-Demo.zip, ESP-IDF/ESP32-S3-LCD-1.47B-Test/main
//   [ARD-DEMO] ESP32-S3-LCD-1.47B-Demo.zip, Arduino/examples/{LCD_Image,LVGL_Arduino}
//   [SCH]      ESP32-S3-LCD-1.47B_schematic_diagram.pdf
#pragma once

// ---- LCD: ST7789 (Waveshare: "ST7789T"), 172x320 IPS, 4-wire SPI ----------
#define BOARD_LCD_PIN_SCLK   40   // [WIKI], [IDF-DEMO] LCD_Driver/ST7789.h, [ARD-DEMO] Display_ST7789.h
#define BOARD_LCD_PIN_MOSI   45   // [WIKI], [IDF-DEMO] ST7789.h, [ARD-DEMO]
#define BOARD_LCD_PIN_CS     42   // [WIKI], [IDF-DEMO] ST7789.h, [ARD-DEMO]
#define BOARD_LCD_PIN_DC     41   // [WIKI], [IDF-DEMO] ST7789.h, [ARD-DEMO]
#define BOARD_LCD_PIN_RST    39   // [WIKI], [IDF-DEMO] ST7789.h, [ARD-DEMO]
#define BOARD_LCD_PIN_BL     46   // [WIKI], [IDF-DEMO] ST7789.h (LEDC PWM, active high)

#define BOARD_LCD_SPI_HOST   SPI3_HOST  // [IDF-DEMO] ST7789.h uses SPI3_HOST (pins are GPIO-matrix routed)
#define BOARD_LCD_SPI_MODE   0          // [IDF-DEMO] io_config.spi_mode = 0, [ARD-DEMO] SPI_MODE0
// [ARD-DEMO] Display_ST7789.h: SPIFreq 80000000. [IDF-DEMO] uses 12 MHz, which
// would cap a full-frame redraw at ~13 fps. Drop to 40 MHz if the image tears
// or shows corrupted pixels.
#define BOARD_LCD_SPI_HZ     (80 * 1000 * 1000)

#define BOARD_LCD_WIDTH      172  // [WIKI], [IDF-DEMO] EXAMPLE_LCD_H_RES, [ARD-DEMO] LCD_WIDTH
#define BOARD_LCD_HEIGHT     320  // [WIKI], [IDF-DEMO] EXAMPLE_LCD_V_RES, [ARD-DEMO] LCD_HEIGHT
#define BOARD_LCD_OFFSET_X   34   // [IDF-DEMO] ST7789.h Offset_X, [ARD-DEMO] Display_ST7789.h Offset_X
#define BOARD_LCD_OFFSET_Y   0    // [IDF-DEMO] ST7789.h Offset_Y, [ARD-DEMO] Offset_Y

// Both demos send INVON (0x21) at the end of init.
#define BOARD_LCD_INVERT     1
// Colour order / mirroring: [ARD-DEMO] writes MADCTL = 0x00 (RGB, no mirror).
// [IDF-DEMO] ends up with MADCTL = 0x48 (BGR | MX). Hardware test: the
// orientation markers appeared in the expected corners (no mirroring), and
// the colour errors were fully explained by byte order alone (see below), so
// RGB order is correct.
#define BOARD_LCD_BGR        0
#define BOARD_LCD_MIRROR_X   0
#define BOARD_LCD_MIRROR_Y   0
// Pixel byte order. Verified on hardware with MODE_TEST_PATTERN: with
// pre-swapped (big-endian) words the panel showed red->blue, green->red,
// blue->green and a broken grey ramp, i.e. it reads pixels little-endian.
// That matches the RAMCTRL value both Waveshare demos send (0xB0: 00 E8,
// ENDIAN bit set) and the ESP-IDF demo feeding LVGL un-swapped colours.
// So the frame buffer holds native little-endian RGB565 words.
#define BOARD_LCD_SWAP_BYTES 0

// Backlight PWM ([IDF-DEMO] ST7789.c uses LEDC low-speed, 13-bit, 4 kHz).
#define BOARD_BL_LEDC_FREQ_HZ 4000

// ---- IMU: QMI8658 on I2C ---------------------------------------------------
#define BOARD_I2C_PIN_SDA    48   // [IDF-DEMO] I2C_Driver.h I2C_Touch_SDA_IO, [ARD-DEMO] I2C_SDA_PIN, [SCH] IMU_SDA = IO48
#define BOARD_I2C_PIN_SCL    47   // [IDF-DEMO] I2C_Driver.h I2C_Touch_SCL_IO, [ARD-DEMO] I2C_SCL_PIN, [SCH] IMU_SCL = IO47
#define BOARD_I2C_HZ         400000  // [IDF-DEMO] I2C_MASTER_FREQ_HZ, [ARD-DEMO]
// [IDF-DEMO] QMI8658.c uses QMI8658_L_SLAVE_ADDRESS (0x6B); [SCH] ties SA0 to GND.
// 0x6A is the SA0-high alternative; the driver probes it as a fallback.
#define BOARD_IMU_ADDR       0x6B
#define BOARD_IMU_ADDR_ALT   0x6A

// ---- RGB LED: one WS2812B ---------------------------------------------------
// [SCH] LED2 "WS2812B-0807", data net RGB_IO = IO38. [IDF-DEMO] RGB.h
// BLINK_GPIO 38 driven via led_strip (RMT); [ARD-DEMO] neopixelWrite(38).
#define BOARD_RGB_PIN        38
#define BOARD_RGB_COUNT      1
// Byte order the LED reads from the data line (the only place channel order
// is handled; colour values everywhere else are plain 0xRRGGBB). 0 = GRB, the
// usual WS2812B order and the led_strip default; 1 = RGB. Hardware (1.1.0
// ledcal): with GRB the firmware sent R5 G90 B0 for green 0x47FF00 and the
// LED lit red, i.e. it took the first byte (green) as red, so this LED reads
// RGB. The ledcal environment starts with a red / green / blue check of this.
#define BOARD_RGB_ORDER_RGB  1

// ---- BOOT button -----------------------------------------------------------
// [SCH] Key2 "BOOT" to GND, 10K pull-up R10 to 3V3, net IO0. RESET (Key1) is
// on CHIP_PU and cannot be read. GPIO0 is a strapping pin, but the straps are
// latched only at a chip / system reset (power-up, RESET, brown-out, RTC
// watchdog; ESP32-S3 datasheet v1.6, "Strapping Pins"), and a deep-sleep wake
// is a core reset (ESP32-S3 TRM v1.2, table 7-1), so a BOOT press can wake
// the chip without entering download mode.
#define BOARD_BOOT_PIN       0    // [IDF-DEMO] Button_Driver.h BOOT_KEY_PIN 0, active low

// ---- IMU interrupts ----------------------------------------------------------
// [SCH] QMI8658 pin 4 INT1 -> net IMU_INT1 -> IO13; pin 9 INT2 -> net
// IMU_INT2 -> IO12 (GPIO table: "IO12 IMU_INT2", "IO13 IMU_INT1"). Both are
// RTC GPIOs (0..21), so they can wake the chip from deep sleep. The Waveshare
// demos do not use them. Wake-on-motion uses INT2: in the older datasheet
// (rev 0.6) INT1 also signals CTRL9 command completion.
#define BOARD_IMU_INT1_PIN   13
#define BOARD_IMU_INT2_PIN   12

// ---- Battery voltage ---------------------------------------------------------
// [SCH] BAT -> R1 200K 1 % -> BAT_ADC -> R4 100K 1 % -> GND, C8 100nF on
// BAT_ADC (C9 100nF on BAT); GPIO table "IO1 BAT_ADC". [IDF-DEMO]
// BAT_Driver.h: ADC1 channel 0 (= GPIO1 on the S3), result x 3. The divider
// sits on BAT before the AO3401, so it reads the cell (or the charger's
// output with no cell) also while USB powers the board. The charger's STAT
// pin only drives the charge LED (LED1, via R3 3K), and VBUS reaches no GPIO:
// firmware cannot see charging or USB directly.
#define BOARD_BAT_ADC_PIN    1

// ---- Power (for reference) -------------------------------------------------
// [SCH] single-cell LiPo on BAT -> ETA6098 switching charger (VBUS in); BAT ->
// AO3401 P-FET (on while VBUS is absent) -> VSYS; USB VBUS -> MBR230LSF
// Schottky -> VSYS. VSYS -> ME6217C33 LDO (EN tied to VSYS, always on) ->
// 3V3. Always-on loads on the battery: the BAT_ADC divider R1 200K + R4 100K
// (to IO1), the LDO's quiescent current, and everything on 3V3: ESP32-S3,
// QMI8658, LCD module (backlight switched by GPIO46 -> SI2302 FET with a 10K
// gate pull-down), WS2812B (VDD on 3V3, no switch; data RGB_IO has a 10K
// pull-up R21 to 3V3), SD card slot. Flash and PSRAM are on VDD_SPI, which
// is off in deep sleep.
