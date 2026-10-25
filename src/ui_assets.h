#pragma once

#include "lvgl.h"

// Static UI state images — the "Boot" (idle), "Wifi" (WiFi connected, no
// speaker yet), "WifiSpeaker" (WiFi + speaker connected, NFC reader not
// yet confirmed) and "Ready" (tap a card) vinyl-record graphics. Embedded
// as plain generated C byte arrays (src/assets/*_jpg.c, see
// src/assets/generate.py) and decoded once here, the same way as
// album_art.c's real album art — see ui_assets.c.

// Call once at boot, after display_init(), before any ui_asset_*() call.
void ui_assets_init(void);

const lv_img_dsc_t *ui_asset_boot(void);
const lv_img_dsc_t *ui_asset_wifi(void);
const lv_img_dsc_t *ui_asset_wifispeaker(void);
const lv_img_dsc_t *ui_asset_ready(void);
