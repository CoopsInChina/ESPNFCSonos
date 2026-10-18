#include "wifi_manager.h"
#include "wifi_creds.h"
#include "app_config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
static const int CONNECTED_BIT = BIT0;
static const int FAIL_BIT      = BIT1;
static int        s_retry_count;
static esp_netif_t *s_netif;
static char        s_ip[24] = {0};

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_CONNECT_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retry connect (%d/%d)", s_retry_count, WIFI_CONNECT_RETRIES);
        } else {
            xEventGroupSetBits(s_events, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;
        // Default modem power-save (WIFI_PS_MIN_MODEM) sleeps the radio
        // between beacon intervals, delaying packet reception — fine for
        // idle polling, but devastating for a sustained HTTPS download
        // (album art was measured at ~5KB/s regardless of file size,
        // the signature of a fixed radio-sleep bottleneck, not variable
        // network conditions). This device is mains-powered, no reason
        // to trade throughput for the radio's own power savings.
        esp_wifi_set_ps(WIFI_PS_NONE);
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

void wifi_manager_init(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

void wifi_manager_begin_connect(void)
{
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"...", WIFI_SSID);
}

bool wifi_manager_wait_connect(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected, ip=%s", s_ip);
        return true;
    }
    ESP_LOGW(TAG, "connect failed/timed out");
    return false;
}

bool wifi_manager_is_connected(void)
{
    return (xEventGroupGetBits(s_events) & CONNECTED_BIT) != 0;
}

const char *wifi_manager_ip(void)
{
    return s_ip;
}
