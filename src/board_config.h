#pragma once

#include "hal/gpio_types.h"   // GPIO_NUM_* constants used throughout this file

// ============================================================
// Hardware pin map — Waveshare ESP32-S3-Touch-LCD-1.85 (no-touch variant),
// confirmed against the real wiki page: https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85
// + UART-bridge RC522 NFC module wired to the board's own UART connector.
// ============================================================

// ---- UART connector (NFC module: RC522 UART bridge) ----
#define NFC_UART_PORT      UART_NUM_1
#define NFC_UART_TX_GPIO   GPIO_NUM_43   // -> RC522 RX
#define NFC_UART_RX_GPIO   GPIO_NUM_44   // <- RC522 TX

// ---- Display: ST77916, QSPI ----
#define LCD_QSPI_HOST      SPI2_HOST
#define LCD_CS_GPIO        GPIO_NUM_21
#define LCD_PCLK_GPIO      GPIO_NUM_40
#define LCD_DATA0_GPIO     GPIO_NUM_46
#define LCD_DATA1_GPIO     GPIO_NUM_45
#define LCD_DATA2_GPIO     GPIO_NUM_42
#define LCD_DATA3_GPIO     GPIO_NUM_41
// LCD_RST is NOT a direct GPIO on this board — it's wired through the
// TCA9554 IO expander as EXIO2 (see tca9554.h/.c and TCA_PIN_LCD_RST
// below). esp_lcd_panel_dev_config_t.reset_gpio_num is set to -1 in
// display.c; the expander drives the real reset pulse before panel init.
#define LCD_BL_GPIO        GPIO_NUM_5
// Dropped from 40MHz: solid colors, a smooth gradient, and the WiFi icon
// all render perfectly, but every real (high-entropy, lots of fine detail)
// JPEG photo shows persistent speckled static — the signature of marginal
// QSPI signal integrity that only shows up under rapidly-varying data.
// Testing whether a slower clock clears it.
#define LCD_PCLK_HZ        (20 * 1000 * 1000)

// ---- I2C bus (IMU / RTC / TCA9554 IO expander — shared bus) ----
#define BOARD_I2C_PORT     I2C_NUM_0
#define BOARD_I2C_SCL_GPIO GPIO_NUM_10
#define BOARD_I2C_SDA_GPIO GPIO_NUM_11
#define BOARD_I2C_FREQ_HZ  400000

// ---- TCA9554PWR IO expander ----
// 0x20 is the standard default address (all address pins grounded) — not
// explicitly confirmed on the wiki page, but universal for this chip and
// what the sibling Grill & Groove board (same chip) uses.
#define TCA9554_I2C_ADDR   0x20
#define TCA_PIN_LCD_RST    2   // EXIO2

// ---- I2S audio out (PCM5101 DAC + amp -> speaker) ----
#define I2S_BCLK_GPIO      GPIO_NUM_48   // Speak_BCK
#define I2S_WS_GPIO        GPIO_NUM_38   // Speak_LRCK
#define I2S_DOUT_GPIO      GPIO_NUM_47   // Speak_DIN
