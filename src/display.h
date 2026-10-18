#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(void);
void      display_set_backlight(bool on);
void      display_set_brightness(int pct);   // 0-100

// Lock/unlock LVGL's internal state before touching any lv_obj_t from a
// task other than the LVGL port task.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);
