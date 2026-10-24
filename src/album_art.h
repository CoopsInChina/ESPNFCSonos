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

// True from the moment a request is accepted until its art is applied (or
// the request is abandoned by a newer one) — lets the UI show a spinner
// instead of a blank screen or the previous track's art while it waits.
bool album_art_is_loading(void);

// Re-applies the last successfully decoded art to img_obj unconditionally
// (unlike album_art_update(), doesn't require a "new" image to be
// pending) — for when the UI switches back to the art view (e.g.
// playback resumed) and the widget's source needs restoring even though
// nothing new has downloaded.
void album_art_reapply(lv_obj_t *img_obj);
