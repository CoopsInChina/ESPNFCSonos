/**
 * Album & NFC Sonos controller — boot sequence and main NFC-tap loop.
 *
 * Flow: boot -> WiFi glyph while connecting -> "No WiFi. Check Settings" if
 * that fails, else -> "No Speaker Found" if discovery fails, else ->
 * "Speaker Connected! <name>" briefly -> fullscreen "now playing" screen
 * that always shows the current album art (a spinner covers it whenever
 * new art is loading). An NFC tap plays a track via Spotify on that
 * speaker; a good tap gets one beep + the spinner while the new art loads,
 * a bad tap (tag present, undecodable) gets two beeps.
 */

#include "app_config.h"
#include "wifi_manager.h"
#include "tca9554.h"
#include "display.h"
#include "ui_boot.h"
#include "ui_color_test.h"
#include "sonos_controller.h"
#include "album_art.h"
#include "nfc_reader.h"
#include "buzzer.h"
#include "globals.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "main";

static lv_obj_t *s_now_playing_scr;
static lv_obj_t *s_art_img;
static lv_obj_t *s_spinner;

static void nfc_loop_task(void *arg);
static void boot_task(void *arg);

// Builds the "now playing" screen's objects but doesn't switch to it yet —
// nfc_loop_task touches s_art_img/s_spinner on every iteration regardless
// of whether a speaker has been found, so they must exist before it starts.
static void now_playing_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    s_art_img = lv_img_create(scr);
    lv_obj_set_size(s_art_img, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_center(s_art_img);
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);   // hidden until the first art actually arrives

    s_spinner = lv_spinner_create(scr, 1000, 90);
    lv_obj_set_size(s_spinner, 64, 64);
    lv_obj_center(s_spinner);
    // visible by default — a loading whirlygig beats a blank screen

    s_now_playing_scr = scr;
}

// Fade from whatever boot/status screen is up into the now-playing screen.
static void now_playing_activate(void)
{
    lv_scr_load_anim(s_now_playing_scr, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, false);
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

    if (display_lock(5000)) {
        ui_boot_show();
        display_unlock();
    }

    bool wifi_ok = wifi_manager_wait_connect(WIFI_CONNECT_TIMEOUT_MS);
    ESP_LOGI(TAG, "WiFi: %s", wifi_ok ? wifi_manager_ip() : "not connected");

    sonos_controller_init();
    bool sonos_ok = false;
    if (wifi_ok) sonos_ok = sonos_controller_discover(SONOS_DISCOVERY_TIMEOUT_MS);
    ESP_LOGI(TAG, "Sonos: %s", sonos_ok ? sonos_active_speaker_name() : "not found");

    album_art_init();
    nfc_reader_init();
    buzzer_init();

    if (display_lock(1000)) {
        now_playing_create();
        display_unlock();
    }

    // Started unconditionally — proving buzzer/NFC comms doesn't depend on
    // a speaker being found, and this is also what polls Sonos state once
    // one is.
    xTaskCreate(nfc_loop_task, "nfc_loop", 8192, NULL, 4, NULL);

    if (!wifi_ok) {
        if (display_lock(1000)) {
            ui_boot_show_message("No WiFi.\nCheck Settings");
            display_unlock();
        }
    } else if (!sonos_ok) {
        if (display_lock(1000)) {
            ui_boot_show_message("No Speaker Found");
            display_unlock();
        }
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "Speaker Connected!\n%s", sonos_active_speaker_name());
        if (display_lock(1000)) {
            ui_boot_show_message(msg);
            display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(2500));   // let the user read it before cutting to album art

        if (display_lock(1000)) {
            now_playing_activate();
            display_unlock();
        }
        sonos_controller_start_polling();
    }

    ESP_LOGI(TAG, "Album & NFC Sonos controller v%s running", FIRMWARE_VERSION);

    vTaskDelete(NULL);   // boot_task's job is done — a FreeRTOS task must never just return
}

static void nfc_loop_task(void *arg)
{
    char last_art_url[512] = {0};
    char tap_uri[256];

    for (;;) {
        nfc_tap_result_t tap = nfc_reader_poll(tap_uri, sizeof(tap_uri));

        if (tap == NFC_TAP_GOOD) {
            buzzer_beep(1);
            if (display_lock(200)) {
                lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
                display_unlock();
            }
            sonos_play_spotify_uri(tap_uri);
        } else if (tap == NFC_TAP_BAD) {
            buzzer_beep(2);
        }

        // Pull whatever's now playing and request fresh art when it changes.
        sonos_track_t track;
        sonos_get_track(&track);
        if (track.valid && strcmp(track.album_art_url, last_art_url) != 0) {
            strncpy(last_art_url, track.album_art_url, sizeof(last_art_url) - 1);
            album_art_request(last_art_url);
        }

        // Whirlygig instead of a blank screen or stale art while new art
        // is in flight, whatever triggered the request.
        if (display_lock(50)) {
            if (album_art_is_loading()) {
                lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            }
            if (album_art_update(s_art_img)) {
                lv_obj_clear_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            }
            display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(NFC_POLL_INTERVAL_MS));
    }
}
