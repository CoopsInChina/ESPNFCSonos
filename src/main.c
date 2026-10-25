/**
 * Album & NFC Sonos controller — boot sequence and main NFC-tap loop.
 *
 * The screen is one of six states, continuously re-evaluated (not just
 * decided once at boot) once past the one-time setup step below:
 *   Setup       — no WiFi credentials saved yet; QR code + setup AP up
 *   Boot        — no WiFi
 *   Wifi        — WiFi connected, no Sonos speaker found
 *   WifiSpeaker — WiFi + speaker connected, NFC reader not yet confirmed
 *   Ready       — WiFi + speaker + NFC all confirmed, nothing playing
 *   Art         — WiFi + speaker + actively playing: fullscreen album art
 * While new art is loading, the Boot vinyl is shown with a small spinner
 * inside its label hole, rather than a blank screen.
 *
 * boot_task() blocks in Setup (if no credentials are saved yet — see
 * setup_portal.c) until the wizard saves WiFi + a chosen Sonos speaker to
 * NVS, then walks Boot -> Wifi -> WifiSpeaker in order as each condition
 * is actually met, holding each on screen for at least
 * BOOT_STEP_MIN_DISPLAY_MS so the progression is visible even when
 * everything connects quickly — without that, the live state machine's
 * first evaluation (once nfc_loop_task starts) could find every
 * condition already true and jump straight to the final state, skipping
 * the intermediate ones entirely. From Ready/Art onward, nfc_loop_task's
 * continuous evaluation takes over for live reacting (pausing, losing
 * WiFi, the speaker or the NFC reader going away, etc.).
 *
 * An NFC tap plays a track via Spotify on that speaker: a good tap plays
 * a short chime sound, a bad tap (tag present, undecodable) gets two
 * buzzer beeps.
 */

#include "app_config.h"
#include "wifi_manager.h"
#include "setup_portal.h"
#include "imu.h"
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
#include "esp_system.h"
#include "lvgl.h"
#include "extra/libs/qrcode/lv_qrcode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "main";

typedef enum {
    UI_MODE_NONE = -1,   // forces the first ui_apply_mode() call to actually apply something
    UI_MODE_SETUP,
    UI_MODE_BOOT,
    UI_MODE_WIFI,
    UI_MODE_WIFI_SPEAKER,
    UI_MODE_READY,
    UI_MODE_LOADING,
    UI_MODE_ART,
} ui_mode_t;

static lv_obj_t *s_art_img;
static lv_obj_t *s_spinner;
static lv_obj_t *s_version_label;
static lv_obj_t *s_setup_qr;   // only exists while UI_MODE_SETUP is active
static ui_mode_t s_ui_mode = UI_MODE_NONE;

static void nfc_loop_task(void *arg);
static void boot_task(void *arg);
static void imu_trigger_task(void *arg);

// Waits out whatever's left of BOOT_STEP_MIN_DISPLAY_MS since step_start_ms
// — a no-op if that much time has already passed on its own.
static void hold_boot_step_minimum(uint32_t step_start_ms)
{
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000ULL) - step_start_ms;
    if (elapsed < BOOT_STEP_MIN_DISPLAY_MS) vTaskDelay(pdMS_TO_TICKS(BOOT_STEP_MIN_DISPLAY_MS - elapsed));
}

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

// Applies whichever of {Boot, Wifi, WifiSpeaker, Ready, loading-vinyl,
// live album art} the current mode calls for. Only touches the display
// when the mode actually changes, except for live art, which is
// re-checked every call so newly-decoded art gets picked up without
// needing its own transition.
static void ui_apply_mode(ui_mode_t mode)
{
    if (mode != s_ui_mode) {
        s_ui_mode = mode;
        // The QR widget only belongs on the Setup screen — tear it down
        // on the way to any other mode rather than leaving it floating
        // over whatever image comes next.
        if (mode != UI_MODE_SETUP && s_setup_qr) {
            lv_obj_del(s_setup_qr);
            s_setup_qr = NULL;
        }
        switch (mode) {
        case UI_MODE_SETUP: {
            lv_img_set_src(s_art_img, ui_asset_setup());
            lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_version_label, LV_OBJ_FLAG_HIDDEN);
            if (!s_setup_qr) {
                // Same cream/ink palette as the rest of the UI (see
                // ui_screen_create()'s spinner colors) — plenty of
                // contrast for a phone camera to read reliably despite
                // not being pure black-on-white.
                s_setup_qr = lv_qrcode_create(lv_scr_act(), UI_SETUP_QR_SIZE,
                                               lv_color_hex(0x363531), lv_color_hex(0xE4E1D0));
                lv_obj_set_pos(s_setup_qr, UI_SETUP_QR_X, UI_SETUP_QR_Y);
            }
            const char *qr_data = setup_portal_qr_data();
            lv_qrcode_update(s_setup_qr, qr_data, strlen(qr_data));
            break;
        }
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
        case UI_MODE_WIFI_SPEAKER:
            lv_img_set_src(s_art_img, ui_asset_wifispeaker());
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
    // Needs to be ready before the setup wizard's Sonos-detect step can
    // possibly run (see below) — harmless to call this early regardless,
    // it only sets up a mutex/queue, no WiFi dependency of its own.
    sonos_controller_init();

    // Start WiFi connecting early so the ~2-3s LVGL startup overlaps with
    // association (same pattern as the sibling Grill & Groove project).
    // False means no credentials are saved yet — falls into the one-time
    // setup wizard below once the display is up, instead of connecting.
    wifi_manager_init();
    bool have_creds = wifi_manager_begin_connect();
    // The IMU re-entry gesture (imu_trigger_task, below) requests this on
    // a deliberate "reconfigure" hold — forces Setup mode on this boot
    // even though real credentials are already saved. Consumed (cleared)
    // here so it only forces Setup once, not on every future boot.
    if (wifi_manager_consume_force_setup_flag()) {
        ESP_LOGI(TAG, "Setup: forced by IMU re-entry gesture");
        have_creds = false;
    }

    ESP_ERROR_CHECK(tca9554_init());   // I2C IO expander — must be up before display_init()

    // Harmless if the chip isn't actually found (imu_init() just returns
    // false, no trigger task started) — stack size confirmed on real
    // hardware: 2048 bytes stack-overflowed on the first log line during
    // calibration testing, ESP_LOGI's %f float formatting pulls in a
    // surprisingly stack-heavy float-to-string path (a well-known
    // ESP-IDF/newlib printf gotcha).
    if (imu_init()) xTaskCreate(imu_trigger_task, "imu_trigger", 4096, NULL, 2, NULL);

    ESP_ERROR_CHECK(display_init());

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
    // Only turn the backlight on once the Boot image is actually loaded —
    // previously this ran right after display_init(), while the panel was
    // still showing whatever was in its own GRAM from before this boot
    // (stale content on a soft reset, since PSRAM/panel memory isn't
    // cleared by that — e.g. a flash of the previous session's album art),
    // for the entire ui_assets_init() JPEG-decode window.
    display_set_backlight(true);

    if (!have_creds) {
        // Factory-fresh (or factory-reset) device: block here until the
        // wizard saves WiFi + a chosen Sonos speaker to NVS. No cancel —
        // this board has no touch/buttons, and there's nothing useful to
        // do without WiFi anyway.
        if (display_lock(1000)) {
            ui_apply_mode(UI_MODE_SETUP);
            display_unlock();
        }
        setup_portal_start();
        ESP_LOGI(TAG, "Setup: waiting for the wizard (AP \"%s\")...", WIFI_AP_SSID);
        while (!setup_portal_is_done()) vTaskDelay(pdMS_TO_TICKS(500));
        // The wizard's own finish handler (setup_portal.c's h_finish)
        // schedules a full device restart shortly after this flips true
        // — a clean reboot, rather than trying to drop setup-AP mode and
        // reconnect within this same running WiFi driver instance, which
        // left the STA silently still associated from the wizard's own
        // test-connect with no new connect/disconnect event ever firing,
        // stuck on the Boot screen indefinitely. Nothing past this point
        // in boot_task will actually run before that reboot happens.
        ESP_LOGI(TAG, "Setup: complete, restarting...");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    uint32_t step_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    bool wifi_ok = have_creds && wifi_manager_wait_connect(WIFI_CONNECT_TIMEOUT_MS);
    ESP_LOGI(TAG, "WiFi: %s", wifi_ok ? wifi_manager_ip() : "not connected");

    // Hold Boot visible for a minimum stretch before moving on, however
    // fast WiFi actually connected — see the file header comment for why.
    hold_boot_step_minimum(step_start_ms);

    if (wifi_ok) {
        if (display_lock(1000)) {
            ui_apply_mode(UI_MODE_WIFI);
            display_unlock();
        }
        step_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }

    bool sonos_ok = false;
    if (wifi_ok) sonos_ok = sonos_controller_discover(SONOS_DISCOVERY_TIMEOUT_MS);
    ESP_LOGI(TAG, "Sonos: %s", sonos_ok ? sonos_active_speaker_name() : "not found");
    if (sonos_ok) sonos_controller_start_polling();

    if (wifi_ok) hold_boot_step_minimum(step_start_ms);

    if (wifi_ok && sonos_ok) {
        if (display_lock(1000)) {
            ui_apply_mode(UI_MODE_WIFI_SPEAKER);
            display_unlock();
        }
        step_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }

    album_art_init();
    nfc_reader_init();
    buzzer_init();

    if (wifi_ok && sonos_ok) hold_boot_step_minimum(step_start_ms);

    // Started unconditionally — proving buzzer/NFC comms doesn't depend on
    // a speaker being found, and this is also what drives the continuous
    // UI state machine and polls Sonos state once a speaker is found.
    xTaskCreate(nfc_loop_task, "nfc_loop", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "Album & NFC Sonos controller v%s running", FIRMWARE_VERSION);

    vTaskDelete(NULL);   // boot_task's job is done — a FreeRTOS task must never just return
}

// Confirmed on real hardware (see app_config.h's IMU_TRIGGER_* comment):
// the deliberate "vertical, rotated 180°" pose reads approximately
// ax=-0.96g, ay=-0.07g, az=+0.09g. Flat (gravity on Z) and correctly-
// oriented vertical (ax positive, not negative) read clearly
// differently, so this only matches the one deliberate pose.
static bool imu_in_setup_trigger_pose(float ax, float ay, float az)
{
    return ax < IMU_TRIGGER_AX_MAX &&
           fabsf(ay) < IMU_TRIGGER_LATERAL_MAX &&
           fabsf(az) < IMU_TRIGGER_LATERAL_MAX;
}

// Re-entry into the setup wizard on an already-configured device: hold
// the trigger pose for IMU_TRIGGER_HOLD_MS, then request Setup mode and
// cleanly reboot into it (wifi_manager_consume_force_setup_flag() picks
// this up in boot_task) — a full reboot rather than trying to tear down
// NFC/Sonos/album-art and drop WiFi back into AP mode within this same
// running instance, the same lesson already learned the hard way getting
// the initial setup wizard's own WiFi-mode transition working reliably.
static void imu_trigger_task(void *arg)
{
    uint32_t pose_since_ms = 0;   // 0 = not currently in the trigger pose
    bool     was_in_pose = false;

    for (;;) {
        float ax, ay, az;
        if (imu_read_accel_g(&ax, &ay, &az)) {
            bool in_pose = imu_in_setup_trigger_pose(ax, ay, az);
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

            if (in_pose && !was_in_pose) {
                pose_since_ms = now;
                ESP_LOGI(TAG, "IMU: entered setup-trigger pose, holding for %dms to confirm", IMU_TRIGGER_HOLD_MS);
            } else if (!in_pose && was_in_pose) {
                ESP_LOGI(TAG, "IMU: left setup-trigger pose");
                pose_since_ms = 0;
            }
            was_in_pose = in_pose;

            if (in_pose && pose_since_ms != 0 && now - pose_since_ms >= IMU_TRIGGER_HOLD_MS) {
                ESP_LOGW(TAG, "IMU: setup-trigger pose held %dms — requesting Setup mode and restarting",
                         IMU_TRIGGER_HOLD_MS);
                wifi_manager_request_setup_on_next_boot();
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_INTERVAL_MS));
    }
}

static void nfc_loop_task(void *arg)
{
    char     last_art_url[512] = {0};
    char     tap_uri[256];
    uint32_t force_loading_until_ms = 0;   // 0 = no tap-triggered override active
    uint32_t ready_since_ms = 0;           // 0 = not currently on an unbroken Ready streak
    int      last_brightness_pct = 100;    // avoids redundant LEDC writes every poll
    bool     cmd_was_pending = false;      // edge-detects sonos_is_cmd_pending() going false
    uint32_t cmd_cleared_ms = 0;           // 0 = not within the post-cmd settle grace window

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
        // so art is ready the instant playback resumes. Deferred while a
        // tap-triggered play is still being set up: album_art_request()'s
        // download holds the shared network mutex for its whole multi-
        // second transfer, which starved sonos_controller's own time-
        // sensitive queue-verification SOAP calls in testing and made a
        // tap that actually succeeded get reported as failed. last_art_url
        // is left unchanged so the request still fires once it's no
        // longer pending.
        sonos_track_t track;
        sonos_get_track(&track);
        if (track.valid && strcmp(track.album_art_url, last_art_url) != 0 && !sonos_is_cmd_pending()) {
            strncpy(last_art_url, track.album_art_url, sizeof(last_art_url) - 1);
            album_art_request(last_art_url);
        }

        // Edge-detect sonos_is_cmd_pending() going false, to cover the gap
        // *after* cmd_task finishes but *before* poll_task's own 2s-interval
        // schedule has had a chance to refresh track.transport_state — right
        // at that edge, the last-known reading can still be the transient
        // STOPPED a queue rebuild reports while setup was in progress, not
        // yet whatever actually follows it (TRANSITIONING/PLAYING/genuinely
        // not playing).
        bool cmd_pending_now = sonos_is_cmd_pending();
        if (cmd_was_pending && !cmd_pending_now) {
            cmd_cleared_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        }
        cmd_was_pending = cmd_pending_now;
        bool in_settle_grace = cmd_cleared_ms != 0 &&
            (uint32_t)(esp_timer_get_time() / 1000ULL) - cmd_cleared_ms < CMD_SETTLE_GRACE_MS;

        // Stop waiting the instant Sonos settles into a non-playing state
        // that isn't still working toward playback — no point riding out
        // the full timeout once we already know it's not going to play
        // (e.g. it went straight to PAUSED_PLAYBACK and is staying there).
        // While a tap-triggered play is still pending, or within the settle
        // grace window right after it finishes, ignore transport state
        // entirely: a queue rebuild reports a real but transient STOPPED
        // before anything has a chance to start, which was cancelling the
        // loading screen (and its spinner) well before setup actually
        // finished and its effects had shown up in a fresh poll.
        bool settled_not_playing = track.valid && !track.is_playing && track.transport_state[0] &&
                                    strcmp(track.transport_state, "TRANSITIONING") != 0 &&
                                    !cmd_pending_now && !in_settle_grace;
        if (settled_not_playing) force_loading_until_ms = 0;

        // While a tap-triggered play is pending, don't trust is_playing as
        // "we've reached the new target" — it's necessarily stale data
        // about whatever was playing *before* this tap, and the forced-
        // loading branch below unconditionally clears force_loading_until_ms
        // once reached. Without this guard, a tap that landed while art was
        // already showing could see that stale is_playing still true for
        // one more poll cycle, wipe out the just-set override immediately,
        // and then fall through to a bare Ready (no spinner) the instant
        // the queue-clear actually took effect and is_playing caught up.
        bool playing_with_art = track.valid && track.is_playing && !album_art_is_loading() &&
                                 !cmd_pending_now;
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
        } else if (!nfc_reader_is_healthy()) {
            // Nothing's actually playing right now, so this is the "idle"
            // choice between WifiSpeaker and Ready — don't invite a tap
            // the reader can't actually service.
            desired = UI_MODE_WIFI_SPEAKER;
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

        // Idle dimming: only while continuously on Ready (nothing playing,
        // nothing actionable on screen). Any other mode — a tap, playback
        // starting, losing WiFi/the speaker — restores full brightness
        // immediately rather than ramping back up, so the screen is never
        // dim right when something actually changed.
        uint32_t now_ms_val = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (desired == UI_MODE_READY) {
            if (ready_since_ms == 0) ready_since_ms = now_ms_val;
            uint32_t ready_elapsed = now_ms_val - ready_since_ms;
            int pct;
            if (ready_elapsed < READY_DIM_IDLE_MS) {
                pct = 100;
            } else {
                uint32_t ramp_elapsed = ready_elapsed - READY_DIM_IDLE_MS;
                pct = (ramp_elapsed >= READY_DIM_RAMP_MS) ? READY_DIM_TARGET_PCT
                      : 100 - (int)((100 - READY_DIM_TARGET_PCT) * ramp_elapsed / READY_DIM_RAMP_MS);
            }
            if (pct != last_brightness_pct) { display_set_brightness(pct); last_brightness_pct = pct; }
        } else {
            ready_since_ms = 0;
            if (last_brightness_pct != 100) { display_set_brightness(100); last_brightness_pct = 100; }
        }

        vTaskDelay(pdMS_TO_TICKS(NFC_POLL_INTERVAL_MS));
    }
}
