#pragma once

#include "lvgl.h"

// Static UI state images — the "Boot" (idle), "Wifi" (connecting to
// speaker) and "Ready" (tap a card) vinyl-record graphics. Embedded as
// JPEGs via src/CMakeLists.txt's EMBED_FILES (source: Assets/ui/*.jpg)
// and decoded once here, the same way as album_art.c's real album art —
// see ui_assets.c.

// Call once at boot, after display_init(), before any ui_asset_*() call.
void ui_assets_init(void);

const lv_img_dsc_t *ui_asset_boot(void);
const lv_img_dsc_t *ui_asset_wifi(void);
const lv_img_dsc_t *ui_asset_ready(void);
