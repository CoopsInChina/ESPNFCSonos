#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================
// Sonos controller — UPnP/SOAP discovery and direct playback.
// Trimmed port of the Grill & Groove sonos_controller.c
// (/Users/ESP32MusicAndMeat/src/sonos_controller.c): kept discovery,
// SOAP transport, track polling and the direct (server-free) Spotify
// play path. Dropped: favourites/NVS management, groups, OTA hooks,
// node-sonos-http-api fallback — none of that applies to this device.
// ============================================================

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char album_art_url[512];
    bool valid;        // true once first poll succeeds
    bool is_playing;   // from AVTransport's GetTransportInfo (CurrentTransportState == PLAYING)
    // Raw CurrentTransportState (e.g. "PLAYING", "TRANSITIONING",
    // "PAUSED_PLAYBACK", "STOPPED") — lets callers tell "still working on
    // it" (TRANSITIONING) apart from "settled, not going to play"
    // (anything else non-playing).
    char transport_state[32];
} sonos_track_t;

// Initialise internal state. Call once at boot, before discovery.
void sonos_controller_init(void);

// Discover Sonos speakers via SSDP. Blocks up to timeout_ms.
// Prefers SONOS_TARGET_SPEAKER_NAME (wifi_creds.h) if found among results.
// Returns true if at least one speaker was found.
bool sonos_controller_discover(uint32_t timeout_ms);

// Start the background track-polling task (call after a successful discover).
void sonos_controller_start_polling(void);

// Thread-safe copy of the currently-known track info (title/artist/art url).
void sonos_get_track(sonos_track_t *out);

const char *sonos_active_speaker_name(void);
bool        sonos_is_connected(void);

// Direct, server-free playback of a "spotify:TYPE:ID" uri (TYPE = track /
// album / playlist), e.g. as decoded from an NFC tag. Non-blocking: queues
// the SOAP sequence onto the internal command task. Returns false if the
// uri isn't a recognised Spotify uri or no speaker is selected.
bool sonos_play_spotify_uri(const char *uri);

// True while a tap-triggered play is being set up on the speaker (queue
// clear/populate + verification). The opportunistic album art prefetch
// should hold off while this is true — its download holds the shared
// network mutex for several seconds and otherwise starves the
// verification SOAP calls above, which previously caused successful taps
// to be misreported as failed.
bool sonos_is_cmd_pending(void);
