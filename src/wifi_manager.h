#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// WiFi STA manager — NVS-held credentials (set by the setup wizard, see
// setup_portal.c), plus the AP-mode + scanning support that wizard needs.
// wifi_creds.h is no longer used for normal operation; see app_config.h.

// Initialise the WiFi stack (netif, event loop, driver). Call once at boot,
// before display_init() so the ~2-3s LVGL startup overlaps with association.
void wifi_manager_init(void);

// Starts connecting in the background using NVS-saved credentials. Returns
// immediately. False (nothing started) if no credentials are saved yet —
// caller should fall into setup mode. On the very first boot after
// flashing, if NVS is empty but wifi_creds.h defines WIFI_SSID, seeds NVS
// from it once (keeps this project's own dev-flashing workflow working
// without forcing every rebuild through the wizard; a real factory-fresh
// device without that header just goes straight to setup mode).
bool wifi_manager_begin_connect(void);

// Block until connected or timeout_ms elapses (retries WIFI_CONNECT_RETRIES
// times). Returns true on success.
bool wifi_manager_wait_connect(uint32_t timeout_ms);

bool        wifi_manager_is_connected(void);
const char *wifi_manager_ip(void);

// True if valid (non-empty SSID) credentials are currently saved in NVS.
bool wifi_manager_has_credentials(void);

// Saves new credentials to NVS — used by the setup wizard once a test
// connection succeeds. Does not itself (re)connect.
void wifi_manager_save_credentials(const char *ssid, const char *password);

// Sets a flag in NVS requesting Setup mode on the next boot even though
// real credentials are already saved — used by the IMU re-entry gesture
// (main.c) to deliberately reconfigure WiFi/speaker on an already-
// working device. Caller still needs to esp_restart() itself; this just
// persists the request so it survives the reboot.
void wifi_manager_request_setup_on_next_boot(void);

// Checks the flag set by wifi_manager_request_setup_on_next_boot() and
// clears it (so it only forces Setup mode once, not on every future
// boot) — call once per boot, right after wifi_manager_begin_connect().
// Returns true if it was set.
bool wifi_manager_consume_force_setup_flag(void);

// ---- Setup-mode AP (see setup_portal.c for the HTTP/DNS servers layered
// on top of this) ----

// Switches to WIFI_MODE_APSTA and brings up the setup AP (WIFI_AP_SSID,
// open, app_config.h) — APSTA rather than AP-only so a scan/test-connect
// can still run concurrently while the AP serves the portal.
void wifi_manager_start_setup_ap(void);

// Drops back to WIFI_MODE_STA, if currently in setup-AP mode.
void wifi_manager_stop_setup_ap(void);

bool wifi_manager_ap_active(void);

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;   // false only for an open (no-password) network
} wifi_scan_result_t;

// Blocking scan for nearby networks (works while the setup AP is up, since
// that puts the radio in APSTA mode). Fills out (caller-owned, out_max
// entries), returns the count actually written.
int wifi_manager_scan(wifi_scan_result_t *out, int out_max);

// Attempts a test STA connection with the given credentials without
// touching NVS or disturbing the setup AP (APSTA mode allows both at
// once) — the setup wizard calls this before persisting anything, so a
// bad password doesn't get saved. Blocks up to timeout_ms. On success,
// ip_out (if non-NULL, ip_out_sz bytes) receives the dotted IP string.
bool wifi_manager_test_connect(const char *ssid, const char *password,
                                uint32_t timeout_ms, char *ip_out, size_t ip_out_sz);
