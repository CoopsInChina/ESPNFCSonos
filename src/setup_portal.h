#pragma once

#include <stdbool.h>

// ============================================================
// WiFi + Sonos speaker setup wizard — captive-portal web server.
//
// Runs only while no WiFi credentials are saved yet (see
// wifi_manager_has_credentials()). Brings up the setup AP
// (wifi_manager_start_setup_ap()), a DNS server that answers every query
// with the AP's own IP so a phone's OS auto-pops the portal, and an HTTP
// server serving a small wizard: scan for WiFi -> pick network + password
// -> test-connect -> scan for Sonos speakers -> pick one -> save
// everything to NVS.
//
// DNS hijack + wildcard-route + socket-pool-exhaustion fixes below are
// carried over from the sibling Grill & Groove project's own captive
// portal (/Users/ESP32MusicAndMeat/src/wifi_manager.c), confirmed on real
// hardware there.
// ============================================================

// Starts the setup AP, DNS server and HTTP server. Call only when
// !wifi_manager_has_credentials(). Idempotent.
void setup_portal_start(void);

// Stops the HTTP/DNS servers and drops the radio back to STA mode. Safe
// to call from any task — unlike calling httpd_stop() from inside one of
// its own request handlers (which self-deadlocks), this is meant to be
// called from the *caller's* polling loop once setup_portal_is_done()
// goes true, not from within a request handler.
void setup_portal_stop(void);

// True once the wizard's last step (POST /api/finish) has successfully
// persisted WiFi credentials + the chosen Sonos speaker to NVS. The
// caller (main.c's boot sequence) polls this, then calls
// setup_portal_stop() and retries the normal connect path.
bool setup_portal_is_done(void);

// A `WIFI:T:nopass;S:<ssid>;;` QR payload for the setup AP — scanning it
// with a phone's camera app joins the AP directly (no browser needed for
// that step), after which the DNS hijack + portal_redirect_handler's
// catch-all route should auto-pop the wizard via the OS's own
// connectivity-check probe.
const char *setup_portal_qr_data(void);
