#pragma once

#include <stdbool.h>
#include <stddef.h>

// ============================================================
// NFC tag reader — RC522 UART-bridge module.
//
// Tag TEXT FORMAT is a known quantity, carried over from the ESP Sonos
// Controller project (/Users/garry.cooper/Sonos/src/pn532_nfc_manager.cpp):
// tags are written with an NDEF Text record whose payload is a bare
// "service:type:id" string, e.g. "spotify:album:2dfTV7CktUEBkZCHiB7VQB" or
// "spotify:track:<id>". That full string IS the uri handed to
// sonos_play_spotify_uri().
//
// Protocol: host-driven command/response, framed as
// 0x7F [cmd] [data...] 0xF7 (0x55 tail on error) — confirmed against a
// working reference library for this exact module
// (github.com/TCL47-GITHUB/RC522_UART). Not auto-reporting: every read is
// us sending REQUEST then ANTICOLL, then SELECT + AUTH + READ to pull the
// NDEF "service:type:id" text off the card. See nfc_reader.c.

typedef enum {
    NFC_TAP_NONE,   // nothing new
    NFC_TAP_GOOD,   // tag present, text decoded as a valid "service:type:id" uri
    NFC_TAP_BAD,    // tag present, but no decodable text (unformatted tag, read error, etc.)
} nfc_tap_result_t;

void nfc_reader_init(void);

// Call periodically (e.g. every NFC_POLL_INTERVAL_MS from the main loop).
// On NFC_TAP_GOOD, uri_out holds the decoded "service:type:id" string.
nfc_tap_result_t nfc_reader_poll(char *uri_out, size_t uri_out_sz);

// True once ANT_ON has been confirmed acknowledged by the module — either
// at boot or via nfc_reader_poll()'s periodic re-assertion. Lets the UI
// distinguish "WiFi + speaker connected, but the NFC reader itself isn't
// confirmed working yet" from genuinely ready to tap.
bool nfc_reader_is_healthy(void);
