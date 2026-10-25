#include "imu.h"
#include "board_config.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "imu";

#define I2C_TIMEOUT_MS 50

// Confirmed from the QMI8658A datasheet (Waveshare's own, not the
// board's wiki page, which only names the chip): SDO/SA0 has an
// internal 200K pull-up enabled during reset, auto-disabled once the
// I2C address is detected — so leaving SA0 unconnected or pulled high
// (the documented default/recommended wiring) gives address 0x6A;
// pulled low gives 0x6B. Which one this specific board's layout uses
// isn't documented, so both are probed.
#define QMI8658_ADDR_SA0_HIGH 0x6A
#define QMI8658_ADDR_SA0_LOW  0x6B

#define REG_WHO_AM_I 0x00
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03
#define REG_CTRL7    0x08
#define REG_AX_L     0x35

#define WHO_AM_I_EXPECTED 0x05

// LSB/g at the +-2g full-scale setting used below: a 16-bit 2's
// complement register covers -32768..32767 over -2g..+2g, i.e. 16384
// LSB per g (32768 / 2).
#define ACCEL_LSB_PER_G 16384.0f

static uint8_t s_addr = 0;   // 0 = not found / not initialised

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(BOARD_I2C_PORT, s_addr, buf, sizeof(buf),
                                       pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(BOARD_I2C_PORT, s_addr, &reg, 1, buf, len,
                                         pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static bool probe_addr(uint8_t addr)
{
    s_addr = addr;
    uint8_t who = 0;
    if (reg_read(REG_WHO_AM_I, &who, 1) != ESP_OK) return false;
    return who == WHO_AM_I_EXPECTED;
}

bool imu_init(void)
{
    if (!probe_addr(QMI8658_ADDR_SA0_HIGH) && !probe_addr(QMI8658_ADDR_SA0_LOW)) {
        ESP_LOGW(TAG, "QMI8658 not found at 0x%02X or 0x%02X",
                 QMI8658_ADDR_SA0_HIGH, QMI8658_ADDR_SA0_LOW);
        s_addr = 0;
        return false;
    }
    ESP_LOGI(TAG, "QMI8658 found at 0x%02X", s_addr);

    // CTRL1: ADDR_AI=1 (bit6, auto-increment — the 6-byte burst read in
    // imu_read_accel_g() depends on this), BE=0 (bit5, little-endian —
    // AX_L/AX_H then combine as a plain 16-bit LE pair, matching their
    // "_L"/"_H" naming directly instead of needing to account for the
    // datasheet's big-endian *default*).
    reg_write(REG_CTRL1, 0x40);
    // CTRL2: accelerometer +-2g (aFS=000), ODR ~62.5Hz normal mode
    // (aODR=0111) — far more than this needs (an occasional orientation
    // check), just a sane low-ish rate rather than the IMU's multi-kHz
    // maximum.
    reg_write(REG_CTRL2, 0x07);
    // CTRL7: aEN=1 (accelerometer on), gEN=0 (gyroscope left disabled —
    // not needed for gravity-vector orientation sensing, saves power).
    reg_write(REG_CTRL7, 0x01);

    return true;
}

bool imu_read_accel_g(float *ax, float *ay, float *az)
{
    if (!s_addr) return false;
    uint8_t raw[6];
    if (reg_read(REG_AX_L, raw, sizeof(raw)) != ESP_OK) return false;

    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);

    *ax = x / ACCEL_LSB_PER_G;
    *ay = y / ACCEL_LSB_PER_G;
    *az = z / ACCEL_LSB_PER_G;
    return true;
}
