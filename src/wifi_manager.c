#include "wifi_manager.h"
#include "wifi_creds.h"   // WIFI_SSID/WIFI_PASSWORD — one-time NVS seed only, see begin_connect()
#include "app_config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
static const int CONNECTED_BIT = BIT0;
static const int FAIL_BIT      = BIT1;
static int        s_retry_count;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static char        s_ip[24] = {0};
static bool        s_ap_active = false;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
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

// ---- NVS credentials ------------------------------------------------

static bool load_credentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = ssid_sz;
    nvs_get_str(nvs, NVS_KEY_WIFI_SSID, ssid, &len);
    len = pass_sz;
    nvs_get_str(nvs, NVS_KEY_WIFI_PASS, pass, &len);
    nvs_close(nvs);
    return ssid[0] != '\0';
}

void wifi_manager_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
        nvs_set_str(nvs, NVS_KEY_WIFI_PASS, password ? password : "");
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "saved credentials for \"%s\"", ssid);
    }
}

bool wifi_manager_has_credentials(void)
{
    char ssid[33] = {0}, pass[64] = {0};
    return load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
}

void wifi_manager_request_setup_on_next_boot(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_FORCE_SETUP, 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

bool wifi_manager_consume_force_setup_flag(void)
{
    nvs_handle_t nvs;
    uint8_t flag = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    nvs_get_u8(nvs, NVS_KEY_FORCE_SETUP, &flag);
    if (flag) {
        nvs_erase_key(nvs, NVS_KEY_FORCE_SETUP);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return flag != 0;
}

// ---- Lifecycle --------------------------------------------------------

void wifi_manager_init(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Started exactly once, here — every later connect attempt
    // (begin_connect(), test_connect()) just calls esp_wifi_connect()
    // directly rather than esp_wifi_start()+WIFI_EVENT_STA_START. Calling
    // esp_wifi_start() again after leaving setup-AP mode (APSTA -> STA is
    // just a mode switch, not a stop/restart — see stop_setup_ap()) was a
    // no-op that never re-fired STA_START, silently leaving the device
    // with updated config but never actually connected — confirmed on
    // real hardware as a device stuck on the Boot screen after a
    // successful setup wizard run, no further logs at all.
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_manager_begin_connect(void)
{
    // One-time dev convenience: a factory-fresh device has nothing in NVS
    // and should go straight to the setup wizard — but on this project's
    // own dev-flashing workflow, wifi_creds.h already has real
    // credentials on disk, and re-running the wizard after every reflash
    // would be pure friction. Seed NVS from it once; a real shipped
    // device without that header just falls through to setup mode.
    if (!wifi_manager_has_credentials() && WIFI_SSID[0]) {
        ESP_LOGI(TAG, "NVS empty — seeding from wifi_creds.h for this dev build");
        wifi_manager_save_credentials(WIFI_SSID, WIFI_PASSWORD);
    }

    char ssid[33] = {0}, pass[64] = {0};
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no saved WiFi credentials");
        return false;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_retry_count = 0;
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to \"%s\"...", ssid);
    return true;
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

// ---- Setup-mode AP ----------------------------------------------------

void wifi_manager_start_setup_ap(void)
{
    if (s_ap_active) return;

    // Bare (unchecked) calls here, matching the sibling Grill & Groove
    // project's own confirmed-working pattern — esp_wifi_stop() can
    // return ESP_ERR_WIFI_NOT_STARTED on a device going straight to setup
    // mode without ever starting STA first, and wrapping the mode switch
    // in ESP_ERROR_CHECK risks aborting on an edge case neither project
    // has actually hit in practice.
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(WIFI_AP_SSID);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_active = true;
    ESP_LOGI(TAG, "setup AP up: \"%s\" http://%s", WIFI_AP_SSID, WIFI_AP_IP);
}

void wifi_manager_stop_setup_ap(void)
{
    if (!s_ap_active) return;
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup AP stopped");
}

bool wifi_manager_ap_active(void)
{
    return s_ap_active;
}

// ---- Scan / test-connect (setup wizard) --------------------------------

int wifi_manager_scan(wifi_scan_result_t *out, int out_max)
{
    wifi_scan_config_t cfg = { .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t scan_err = esp_wifi_scan_start(&cfg, true);   // blocking, ~2s
    if (scan_err != ESP_OK) {
        // Confirmed on the sibling Grill & Groove project: this can fail
        // outright in concurrent AP+STA mode (e.g. ESP_ERR_WIFI_STATE)
        // with no other symptom — silently ignoring it just produced an
        // empty list with no clue why.
        ESP_LOGW(TAG, "scan: esp_wifi_scan_start failed: %s", esp_err_to_name(scan_err));
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > (uint16_t)out_max) count = (uint16_t)out_max;
    if (count == 0) return 0;

    wifi_ap_record_t *list = malloc(count * sizeof(wifi_ap_record_t));
    if (!list) return 0;
    esp_wifi_scan_get_ap_records(&count, list);

    int n = 0;
    for (int i = 0; i < count; i++) {
        if (!list[i].ssid[0]) continue;
        strncpy(out[n].ssid, (const char *)list[i].ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        out[n].rssi   = list[i].rssi;
        out[n].secure = list[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    free(list);
    return n;
}

bool wifi_manager_test_connect(const char *ssid, const char *password,
                                uint32_t timeout_ms, char *ip_out, size_t ip_out_sz)
{
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password) strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_retry_count = 0;
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) { ESP_LOGW(TAG, "test_connect: set_config failed: %s", esp_err_to_name(err)); return false; }
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & CONNECTED_BIT) {
        if (ip_out) strncpy(ip_out, s_ip, ip_out_sz - 1);
        return true;
    }
    return false;
}
