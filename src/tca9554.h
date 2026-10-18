#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// TCA9554PWR I2C IO expander — on this board, the LCD's hardware RESET line
// is wired through here (EXIO2), not a direct ESP32 GPIO. Ported from
// /Users/ESP32MusicAndMeat/src/tca9554.c (same chip, different board pins).

// Initialise I2C master and the TCA9554PWR expander.
// All 8 pins are configured as outputs, initially high.
esp_err_t tca9554_init(void);

// Set a single pin (1-based, 1..8 - "EXIOn" labels on the schematic map
// directly to n here) to high (true) or low (false), without disturbing
// the other pins.
esp_err_t tca9554_set_pin(uint8_t pin, bool level);
