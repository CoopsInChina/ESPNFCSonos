/**
 * Album & NFC Sonos controller — boot sequence and main NFC-tap loop.
 *
 * The screen is one of four states, continuously re-evaluated (not just
 * decided once at boot):
 *   Boot  — no WiFi
 *   Wifi  — WiFi connected, no Sonos speaker found
 *   Ready — WiFi + speaker, but nothing actively playing (idle/paused)
 *   Art   — WiFi + speaker + actively playing: fullscreen album art
 * While new art is loading, the Boot vinyl is shown with a small spinner
 * inside its label hole, rather than a blank screen.
 *
 * An NFC tap plays a track via Spotify on that speaker: a good tap plays
 * a short chime sound, a bad tap (tag present, undecodable) gets two
 * buzzer beeps.
 */

#include "app_config.h"
#include "wifi_manager.h"
#include "tca9554.h"
#include "display.h"
#include "ui_assets.h"
#include "ui_color_test.h"
#include "sonos_controller.h"
#include "album_art.h"
#include "nfc_reader.h"
#include "buzzer.h"
#include "globals.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "main";

typedef enum {
    UI_MODE_NONE = -1,   // forces the first ui_apply_mode() call to actually apply something
    UI_MODE_BOOT,
    UI_MODE_WIFI,
    UI_MODE_READY,
    UI_MODE_LOADING,
    UI_MODE_ART,
} ui_mode_t;

static lv_obj_t *s_art_img;
static lv_obj_t *s_spinner;
static lv_obj_t *s_version_label;
static ui_mode_t s_ui_mode = UI_MODE_NONE;

static void nfc_loop_task(void *arg);
static void boot_task(void *arg);

static void ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    s_art_img = lv_img_create(scr);
    lv_obj_set_size(s_art_img, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_center(s_art_img);
    lv_img_set_src(s_art_img, ui_asset_boot());   // shown immediately, before WiFi/speaker are known

    // Sized to sit inside the vinyl graphics' label hole (~61px at this
    // display size) with a little margin. The hole's true center (sampled
    // directly from boot.jpg) is ~3px above the image's exact geometric
    // center, hence the -3 y-offset instead of a plain lv_obj_center().
    s_spinner = lv_spinner_create(scr, 1000, 90);
    lv_obj_set_size(s_spinner, 56, 56);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, -1, -3);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    // Default theme colors (blue, plus an accent-colored knob at the
    // arc's leading edge) clash with the vinyl artwork — recolor to
    // match it instead: track = the label hole's cream, moving arc =
    // the vinyl's near-black (both sampled directly from boot.jpg), knob
    // hidden entirely rather than guessing a third color for it.
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0xE4E1D0), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x363531), LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_spinner, LV_OPA_TRANSP, LV_PART_KNOB);

    // Version number, boot screen only — same position as the baked-in
    // "TAP A CARD" text on ready.jpg (sampled directly from Ready.png: the
    // text sits ~80% down the image), same ink color as the vinyl's print.
    s_version_label = lv_label_create(scr);
    lv_label_set_text(s_version_label, "V" FIRMWARE_VERSION);
    lv_obj_set_style_text_font(s_version_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_version_label, lv_color_hex(0x363531), 0);
    lv_obj_align(s_version_label, LV_ALIGN_BOTTOM_MID, 0, -72);

    lv_scr_load(scr);
}

// Applies whichever of {Boot, Wifi, Ready, loading-vinyl, live album art}
// the current mode calls for. Only touches the display when the mode
// actually changes, except for live art, which is re-checked every call
// so newly-decoded art gets picked up without needing its own transition.
static void ui_apply_mode(ui_mode_t mode)
{
    if (mode != s_ui_mode) {
        s_ui_mode = mode;
        switch (mode) {
        case UI_MODE_BOOT:
            lv_img_set_src(s_art_img, ui_asset_boot());
            lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_version_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_MODE_WIFI:
            lv_img_set_src(s_art_img, ui_asset_wifi());
            lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_version_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_MODE_READY:
            lv_img_set_src(s_art_img, ui_asset_ready());
            lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_version_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_MODE_LOADING:
            // Per spec: the loading whirlygig sits inside the boot vinyl,
            // not a blank screen.
            lv_img_set_src(s_art_img, ui_asset_boot());
            lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_version_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_MODE_ART:
            lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_version_label, LV_OBJ_FLAG_HIDDEN);
            album_art_reapply(s_art_img);   // restore real art if returning from Ready/Loading
            break;
        default:
            break;
        }
    }
    if (mode == UI_MODE_ART) album_art_update(s_art_img);   // swap in newly-decoded art whenever it arrives
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Everything else runs on its own dedicated task/stack rather than the
    // shared "main" task — sonos_controller_discover() alone has >2KB of
    // its own stack-local buffers (SSDP receive buffers), which overflowed
    // the main task's stack the first time this code path actually ran
    // (WiFi had failed every time before, short-circuiting past it).
    xTaskCreate(boot_task, "boot", 8192, NULL, 5, NULL);
}

static void boot_task(void *arg)
{
    globals_init();

    // Start WiFi connecting early so the ~2-3s LVGL startup overlaps with
    // association (same pattern as the sibling Grill & Groove project).
    wifi_manager_init();
    wifi_manager_begin_connect();

    ESP_ERROR_CHECK(tca9554_init());   // I2C IO expander — must be up before display_init()
    ESP_ERROR_CHECK(display_init());
    display_set_backlight(true);

#if 0   // DIAGNOSTIC — rendering path confirmed clean (swatches + gradient + WiFi icon all perfect); re-enable if needed
    if (display_lock(5000)) {
        ui_color_test_show();
        display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(8000));

    // Static shows on every real album art image even when JPEG decode
    // logs success — this pushes a synthetic gradient through the exact
    // same raw lv_img_dsc_t path album_art.c uses, with zero JPEG
    // involved, to isolate rendering-path bugs from decoder bugs.
    if (display_lock(5000)) {
        ui_color_test_show_gradient();
        display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(15000));
#endif

    ui_assets_init();

    if (display_lock(1000)) {
        ui_screen_create();   // shows the Boot vinyl immediately
        display_unlock();
    }

    bool wifi_ok = wifi_manager_wait_connect(WIFI_CONNECT_TIMEOUT_MS);
    ESP_LOGI(TAG, "WiFi: %s", wifi_ok ? wifi_manager_ip() : "not connected");

    sonos_controller_init();
    bool sonos_ok = false;
    if (wifi_ok) sonos_ok = sonos_controller_discover(SONOS_DISCOVERY_TIMEOUT_MS);
    ESP_LOGI(TAG, "Sonos: %s", sonos_ok ? sonos_active_speaker_name() : "not found");
    if (sonos_ok) sonos_controller_start_polling();

    album_art_init();
    nfc_reader_init();
    buzzer_init();

    // Started unconditionally — proving buzzer/NFC comms doesn't depend on
    // a speaker being found, and this is also what drives the continuous
    // UI state machine and polls Sonos state once a speaker is found.
    xTaskCreate(nfc_loop_task, "nfc_loop", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "Album & NFC Sonos controller v%s running", FIRMWARE_VERSION);

    vTaskDelete(NULL);   // boot_task's job is done — a FreeRTOS task must never just return
}

static void nfc_loop_task(void *arg)
{
    char     last_art_url[512] = {0};
    char     tap_uri[256];
    uint32_t force_loading_until_ms = 0;   // 0 = no tap-triggered override active

    for (;;) {
        nfc_tap_result_t tap = nfc_reader_poll(tap_uri, sizeof(tap_uri));

        if (tap == NFC_TAP_GOOD) {
            buzzer_play_tap_sound();
            sonos_play_spotify_uri(tap_uri);
            // Instant feedback: actual playback starting (and the track
            // poll catching up to it) lags behind the tap by up to a
            // couple of seconds, so force the loading screen now rather
            // than waiting for the state machine below to notice.
            force_loading_until_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) + TAP_LOADING_TIMEOUT_MS;
        } else if (tap == NFC_TAP_BAD) {
            buzzer_beep(2);
        }

        // Pull whatever's now playing and request fresh art when it
        // changes — kept up to date in the background even while paused,
        // so art is ready the instant playback resumes.
        sonos_track_t track;
        sonos_get_track(&track);
        if (track.valid && strcmp(track.album_art_url, last_art_url) != 0) {
            strncpy(last_art_url, track.album_art_url, sizeof(last_art_url) - 1);
            album_art_request(last_art_url);
        }

        // Stop waiting the instant Sonos settles into a non-playing state
        // that isn't still working toward playback — no point riding out
        // the full timeout once we already know it's not going to play
        // (e.g. it went straight to PAUSED_PLAYBACK and is staying there).
        bool settled_not_playing = track.valid && !track.is_playing && track.transport_state[0] &&
                                    strcmp(track.transport_state, "TRANSITIONING") != 0;
        if (settled_not_playing) force_loading_until_ms = 0;

        bool playing_with_art = track.valid && track.is_playing && !album_art_is_loading();
        bool forced_loading   = force_loading_until_ms != 0 &&
                                 (uint32_t)(esp_timer_get_time() / 1000ULL) < force_loading_until_ms;
        // Sonos reports TRANSITIONING while actively working toward a new
        // track — not just for our own tap-triggered plays, but for any
        // track change (e.g. selected from the Sonos app, or auto-advance).
        // Without this, those showed Ready for a few seconds until
        // is_playing caught up, instead of the loading screen the whole
        // time something's actually changing.
        bool transitioning = track.valid && strcmp(track.transport_state, "TRANSITIONING") == 0;

        ui_mode_t desired;
        if (!wifi_manager_is_connected()) {
            desired = UI_MODE_BOOT;
            force_loading_until_ms = 0;
        } else if (!sonos_is_connected()) {
            desired = UI_MODE_WIFI;
            force_loading_until_ms = 0;
        } else if (playing_with_art) {
            desired = UI_MODE_ART;
            force_loading_until_ms = 0;   // reached it — tap override no longer needed
        } else if (forced_loading || transitioning || (track.is_playing && album_art_is_loading())) {
            // Tap override, actively transitioning, or actively playing
            // but waiting on new art all earn the loading screen —
            // background prefetch for a stopped/paused leftover track
            // (e.g. right after boot) must not, with nothing actually
            // about to show.
            desired = UI_MODE_LOADING;
        } else {
            desired = UI_MODE_READY;   // tap override (if any) timed out without ever reaching Art
        }

        if (display_lock(50)) {
            ui_apply_mode(desired);
            display_unlock();
        } else if (desired != s_ui_mode) {
            // Lost a mode change to lock contention — worth knowing about
            // if the UI ever seems to "miss" a transition (e.g. spinner
            // never appearing despite the state machine wanting it).
            ESP_LOGW(TAG, "display_lock busy, dropped mode change %d -> %d", s_ui_mode, desired);
        }

        vTaskDelay(pdMS_TO_TICKS(NFC_POLL_INTERVAL_MS));
    }
}
