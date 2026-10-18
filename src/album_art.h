#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

// Fullscreen album art: download -> JPEG decode -> bilinear scale -> PSRAM
// RGB565 buffer. Simplified single-image port of the sibling project's
// ui_art.c (/Users/ESP32MusicAndMeat/src/ui_art.c) — no multi-slot
// favourites cache, since this device only ever shows "whatever is
// playing now".

// Call once from app_main, after display is up.
void album_art_init(void);

// Request art for a raw Sonos/Spotify album art URL. Returns immediately;
// download + decode happens in a background task. Pass "" to cancel.
void album_art_request(const char *raw_url);

// Call from the LVGL task (e.g. a timer). Applies newly-decoded art to
// img_obj if ready. Returns true when art was applied this call.
bool album_art_update(lv_obj_t *img_obj);
