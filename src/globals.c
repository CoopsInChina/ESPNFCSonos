#include "globals.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/task.h"

#define NET_COOLDOWN_MS 200   // matches the sibling project's SDIO_GENERAL_COOLDOWN_MS

static const char *TAG = "net_guard";

SemaphoreHandle_t g_network_mutex;
volatile uint32_t g_last_network_end_ms;

void globals_init(void)
{
    g_network_mutex = xSemaphoreCreateMutex();
}

void net_pre_wait(const char *tag)
{
    if (g_last_network_end_ms == 0) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed = now - g_last_network_end_ms;
    if (elapsed < NET_COOLDOWN_MS) {
        uint32_t wait = NET_COOLDOWN_MS - elapsed;
        ESP_LOGD(TAG, "[%s] cooldown: waiting %ums", tag, (unsigned)wait);
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}
