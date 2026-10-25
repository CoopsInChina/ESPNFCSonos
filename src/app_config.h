#pragma once

// ============================================================
// Album & NFC Sonos controller — application configuration
// Spinoff of Grill & Groove (/Users/ESP32MusicAndMeat) and the ESP Sonos
// Controller (/Users/garry.cooper/Sonos), scoped down to: boot, show WiFi
// status, find a Sonos speaker, then do nothing but album art + NFC taps.
// ============================================================

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.1"
#endif

// ---- Display ----
// TODO: confirm against the real board — see board_config.h. Placeholder
// assumes a 360x360 round panel (ST77916-based Waveshare 1.85" family).
#define DISPLAY_WIDTH   360
#define DISPLAY_HEIGHT  360

// ---- WiFi STA ----
// Credentials live in wifi_creds.h (plain header, build-time only — no
// captive portal / web server, per explicit request). Do not commit real
// credentials to version control.
#define WIFI_CONNECT_TIMEOUT_MS  15000
#define WIFI_CONNECT_RETRIES     3

// ---- Sonos ----
#define SONOS_DISCOVERY_TIMEOUT_MS  8000
#define SONOS_POLL_INTERVAL_MS      2000
#define SONOS_MAX_SPEAKERS          16

// How long a good NFC tap forces the loading screen, in case actual
// playback starting (and the track/art info catching up) lags behind the
// tap itself — SONOS_POLL_INTERVAL_MS alone means that can take a couple
// of seconds. Falls back to the normal Ready/Art state once this expires
// if nothing ever actually started playing.
#define TAP_LOADING_TIMEOUT_MS      20000

// How long after sonos_is_cmd_pending() clears (cmd_task finished the
// queue setup) to keep ignoring a "not playing" transport_state reading
// as stale rather than genuinely settled. poll_task only refreshes that
// state every SONOS_POLL_INTERVAL_MS on its own schedule, so right after
// setup finishes the last-known reading can still be the transient STOPPED
// seen *during* setup, not yet whatever comes after — needs to cover at
// least one full poll cycle plus call latency.
#define CMD_SETTLE_GRACE_MS         4000

// Minimum time each of the Boot/Wifi/WifiSpeaker boot-sequence screens
// stays up before moving to the next step, even if the underlying
// condition (WiFi connects, speaker is found) resolves faster than that
// — without this, a fast connection can make a step flash by too
// quickly to actually see.
#define BOOT_STEP_MIN_DISPLAY_MS    2000

// Spotify service parameters for direct (server-free) playback, same
// scheme as Grill & Groove's sonos_controller.c: sid = Spotify service id
// on this Sonos household, sn = account serial. Only individual tracks
// need sn; albums/playlists need only sid. Learned at runtime from any
// currently-playing Spotify track if available; these are the cold-start
// defaults (copy the values from the working sibling project's
// app_config.h — same household).
#define SPOTIFY_DEFAULT_SID  12
#define SPOTIFY_DEFAULT_SN   5

// ---- Album art ----
// 280KB measured too tight for a real-world Spotify cover (one download
// hit exactly this limit at 286719 bytes and got truncated, taking 60s
// because the handler kept silently discarding data past the limit
// instead of aborting — see album_art.c's art_dl_evt). Raised with
// headroom; PSRAM has plenty of room for this.
#define ART_MAX_DOWNLOAD_BYTES  (768 * 1024)
#define ART_TASK_STACK          8192

// ---- NFC (RC522, UART bridge module) ----
// Confirmed from the manufacturer's own listing for this exact board
// (same layout, "TestPoint" label, 27.120 crystal): interface is UART
// TTL, fixed baud 115200 — a bridging-MCU product with its own protocol,
// not the bare MFRC522 chip's native 9600-default register interface.
#define NFC_UART_BAUD        115200
// Idle sampling rate only — how often we ask "is a card here?" when
// nothing's present. Independent of the one-off per-tag SELECT/AUTH/READ
// cost once a card IS detected, and of ANTENNA_REASSERT_INTERVAL_MS (see
// nfc_reader.c). At 115200 baud a "no card" REQUEST round-trip is on the
// order of a few ms, comfortably under 100ms; a 300ms interval meant a
// quick tap-and-lift under ~300ms dwell could be missed between polls.
#define NFC_POLL_INTERVAL_MS 100

// ---- Buzzer (I2S tone through the onboard PCM5101 DAC — no discrete
// piezo buzzer on this board, see buzzer.c) ----
#define BEEP_FREQ_HZ       1800
#define BEEP_DURATION_MS   120
#define BEEP_GAP_MS        100   // gap between the two beeps of a bad-tap

// ---- Screen dimming ----
// Idle dimming only on the Ready screen (nothing playing, nothing to read
// that benefits from full brightness) — any other mode restores full
// brightness immediately. 100% -> 20% linearly over the ramp duration,
// i.e. 20% per second / 2% per 100ms.
#define READY_DIM_IDLE_MS      30000   // time on Ready before the ramp starts
#define READY_DIM_RAMP_MS      4000    // ramp duration
#define READY_DIM_TARGET_PCT   5       // floor brightness once ramped down

// ---- NVS ----
#define NVS_NAMESPACE        "album_nfc"
#define NVS_KEY_SPEAKER_IP   "spkr_ip"
#define NVS_KEY_SPEAKER_NAME "spkr_name"
