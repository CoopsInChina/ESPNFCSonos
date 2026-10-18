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
// ==================== OPEN QUESTION — READ ME =========================
// The RC522 module on hand is a UART-BRIDGE variant (a plain MFRC522 +
// its own onboard MCU exposing UART instead of SPI), not the PN532 the
// Sonos Controller project used. We do NOT yet know this exact module's
// wire protocol (what bytes it sends when a card taps, whether it
// auto-reports the UID/NDEF payload or needs a request command first).
// nfc_reader_poll() below has the UART plumbing wired up (correct baud
// rate, correct pins per board_config.h) but parse_frame() is a stub —
// next step is capturing real bytes off the module with a tap (e.g. a
// throwaway sketch that just logs every byte received) and filling in
// the actual framing here.
// ========================================================================

typedef enum {
    NFC_TAP_NONE,   // nothing new
    NFC_TAP_GOOD,   // tag present, text decoded as a valid "service:type:id" uri
    NFC_TAP_BAD,    // tag present, but no decodable text (unformatted tag, read error, etc.)
} nfc_tap_result_t;

void nfc_reader_init(void);

// Call periodically (e.g. every NFC_POLL_INTERVAL_MS from the main loop).
// On NFC_TAP_GOOD, uri_out holds the decoded "service:type:id" string.
nfc_tap_result_t nfc_reader_poll(char *uri_out, size_t uri_out_sz);
