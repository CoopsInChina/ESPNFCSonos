#pragma once

#include <stdbool.h>

// ============================================================
// QMI8658 6-axis IMU — onboard, shared I2C bus with the TCA9554 expander
// (BOARD_I2C_PORT/SCL/SDA in board_config.h, no extra wiring). Confirmed
// present on this board via Waveshare's own wiki page (not wired up by
// any earlier phase of this project); register map confirmed from the
// manufacturer's own QMI8658A datasheet (also not in the board's wiki).
//
// Accelerometer only for now — this project only needs a gravity vector
// for orientation detection (see main.c's planned setup re-entry
// gesture), not the gyroscope.
// ============================================================

// Call once at boot, after tca9554_init() (the I2C bus must already be
// installed — this reuses it, does not install its own). Probes both
// possible I2C addresses (0x6A/0x6B — the SA0 strap isn't documented for
// this specific board) and confirms WHO_AM_I. Returns false if neither
// address responds as a QMI8658.
bool imu_init(void);

// Reads the current raw accelerometer vector, in g (+-2g range, the
// finest resolution — plenty for gravity/orientation sensing). Returns
// false on I2C read failure or if imu_init() never found the chip.
bool imu_read_accel_g(float *ax, float *ay, float *az);
