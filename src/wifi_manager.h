#pragma once

#include <stdbool.h>
#include <stdint.h>

// Minimal WiFi STA manager — build-time credentials only (wifi_creds.h),
// no captive portal / web server.

// Initialise the WiFi stack (netif, event loop, driver). Call once at boot,
// before display_init() so the ~2-3s LVGL startup overlaps with association.
void wifi_manager_init(void);

// Start connecting in the background using WIFI_SSID/WIFI_PASSWORD from
// wifi_creds.h. Returns immediately.
void wifi_manager_begin_connect(void);

// Block until connected or timeout_ms elapses (retries WIFI_CONNECT_RETRIES
// times). Returns true on success.
bool wifi_manager_wait_connect(uint32_t timeout_ms);

bool        wifi_manager_is_connected(void);
const char *wifi_manager_ip(void);
