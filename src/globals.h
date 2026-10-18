#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>

// Shared network mutex — serializes SOAP calls (sonos_controller) and art
// downloads (album_art) so they don't fight over WiFi/heap at the same time.
// Same pattern as the sibling Grill & Groove project.
extern SemaphoreHandle_t g_network_mutex;

// Timestamp (ms) of the last network operation's end — written by
// send_soap()/download_url() right after releasing g_network_mutex, read
// by net_pre_wait() before starting the next one.
extern volatile uint32_t g_last_network_end_ms;

// Minimum gap between any two HTTP/SOAP operations. Prevents a new
// connection's SYN from colliding with the previous one's FIN-ACK on the
// same WiFi RX path — the sibling Grill & Groove project found this
// necessary (as SDIO_GENERAL_COOLDOWN_MS) and the same TCP-level race
// applies here too, independent of their SDIO-specific architecture.
void net_pre_wait(const char *tag);

void globals_init(void);
