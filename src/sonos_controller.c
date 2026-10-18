/**
 * Sonos controller — trimmed ESP-IDF port.
 * Source: /Users/ESP32MusicAndMeat/src/sonos_controller.c (itself a C port
 * of the Arduino SonosESP controller at /Users/garry.cooper/Sonos).
 * Kept: SSDP discovery, SOAP transport, track polling, direct Spotify play.
 * Dropped: favourites/NVS, groups, OTA hooks, node-sonos-http-api fallback.
 */

#include "sonos_controller.h"
#include "globals.h"
#include "app_config.h"
#include "wifi_creds.h"   // SONOS_TARGET_SPEAKER_NAME

#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "sonos";

#define MAX_DEVICES       SONOS_MAX_SPEAKERS
#define SOAP_BODY_BUF_SZ  3072
#define SOAP_RESP_BUF_SZ  4096

static inline uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

typedef struct {
    char name[64];
    char ip[24];
    char uuid[48];
    int  error_count;
    bool connected;
} sonos_speaker_t;

static sonos_speaker_t s_speakers[MAX_DEVICES];
static int             s_speaker_count = 0;
static int             s_active_idx    = -1;

static sonos_track_t     s_track = {0};
static SemaphoreHandle_t s_track_mutex = NULL;

// ---- Command queue: NFC tap -> direct Spotify play, run off the UI/NFC task ----
typedef struct { char uri[256]; } sonos_cmd_t;
static QueueHandle_t  s_cmd_queue = NULL;
static TaskHandle_t   s_poll_task = NULL;
static TaskHandle_t   s_cmd_task_handle = NULL;

static int s_spotify_sid = SPOTIFY_DEFAULT_SID;
static int s_spotify_sn  = SPOTIFY_DEFAULT_SN;

// ---- Small XML helpers ----------------------------------------------

static bool extract_xml(const char *xml, const char *tag, char *out, size_t out_sz)
{
    char open[80], close[84];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *s = strstr(xml, open);
    if (!s) return false;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return false;

    size_t len = (size_t)(e - s);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, s, len);
    out[len] = '\0';
    return true;
}

static void xml_encode(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 6 < dst_sz; p++) {
        switch (*p) {
            case '&':  memcpy(dst + o, "&amp;",  5); o += 5; break;
            case '<':  memcpy(dst + o, "&lt;",   4); o += 4; break;
            case '>':  memcpy(dst + o, "&gt;",   4); o += 4; break;
            case '"':  memcpy(dst + o, "&quot;", 6); o += 6; break;
            case '\'': memcpy(dst + o, "&apos;", 6); o += 6; break;
            default:   dst[o++] = *p; break;
        }
    }
    dst[o] = '\0';
}

static void decode_html_entities(char *s)
{
    static const struct { const char *from; const char *to; } ent[] = {
        { "&amp;",  "&"  }, { "&lt;",   "<"  }, { "&gt;",   ">"  },
        { "&quot;", "\"" }, { "&apos;", "'"  }, { "&#39;",  "'"  },
    };
    for (int i = 0; i < (int)(sizeof(ent)/sizeof(ent[0])); i++) {
        char *p;
        size_t flen = strlen(ent[i].from), tlen = strlen(ent[i].to);
        while ((p = strstr(s, ent[i].from)) != NULL) {
            memmove(p + tlen, p + flen, strlen(p + flen) + 1);
            memcpy(p, ent[i].to, tlen);
        }
    }
}

// ---- HTTP plumbing ---------------------------------------------------

typedef struct { char *buf; int len; int max; } http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (!ctx || !ctx->buf) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (ctx->len + copy > ctx->max - 1) copy = ctx->max - 1 - ctx->len;
        if (copy > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, copy);
            ctx->len += copy;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

// ---- SOAP request -----------------------------------------------------

static int send_soap(const char *service, const char *action,
                     const char *args, char *resp_buf, size_t resp_sz)
{
    if (s_active_idx < 0 || s_active_idx >= s_speaker_count) return -1;
    const char *ip = s_speakers[s_active_idx].ip;

    const char *endpoint;
    static EXT_RAM_BSS_ATTR char custom_ep[128];
    if (strstr(service, "AVTransport"))
        endpoint = "/MediaRenderer/AVTransport/Control";
    else if (strstr(service, "RenderingControl"))
        endpoint = "/MediaRenderer/RenderingControl/Control";
    else if (strstr(service, "ContentDirectory"))
        endpoint = "/MediaServer/ContentDirectory/Control";
    else {
        snprintf(custom_ep, sizeof(custom_ep), "/MediaRenderer/%s/Control", service);
        endpoint = custom_ep;
    }

    static EXT_RAM_BSS_ATTR char url[256];
    snprintf(url, sizeof(url), "http://%s:1400%s", ip, endpoint);

    static EXT_RAM_BSS_ATTR char body[SOAP_BODY_BUF_SZ];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%s>"
        "</s:Body></s:Envelope>",
        action, service, args, action);

    static EXT_RAM_BSS_ATTR char soap_action[280];
    snprintf(soap_action, sizeof(soap_action),
             "\"urn:schemas-upnp-org:service:%s:1#%s\"", service, action);

    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "SOAP: mutex timeout for %s.%s", service, action);
        return -1;
    }
    net_pre_wait("SOAP");

    static EXT_RAM_BSS_ATTR char resp_tmp[SOAP_RESP_BUF_SZ];
    http_ctx_t ctx = { resp_tmp, 0, (int)sizeof(resp_tmp) };
    resp_tmp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 3000,
        .event_handler = http_event_handler,
        .user_data     = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "SOAPAction",   soap_action);
    esp_http_client_set_header(client, "Connection",   "close");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err  = esp_http_client_perform(client);
    int       code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    g_last_network_end_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    xSemaphoreGive(g_network_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SOAP HTTP err %d for %s.%s", err, service, action);
        s_speakers[s_active_idx].error_count++;
        if (s_speakers[s_active_idx].error_count > 5)
            s_speakers[s_active_idx].connected = false;
        return -1;
    }
    if (code == 200) {
        s_speakers[s_active_idx].error_count = 0;
        s_speakers[s_active_idx].connected   = true;
        if (resp_buf && resp_sz > 0) {
            strncpy(resp_buf, resp_tmp, resp_sz - 1);
            resp_buf[resp_sz - 1] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "SOAP %d for %s.%s", code, service, action);
        s_speakers[s_active_idx].error_count++;
    }
    return code;
}

static void fetch_room_name(const char *ip, char *name_buf, size_t name_sz,
                            char *uuid_buf, size_t uuid_sz)
{
    static EXT_RAM_BSS_ATTR char url[128];
    snprintf(url, sizeof(url), "http://%s:1400/xml/device_description.xml", ip);

    static EXT_RAM_BSS_ATTR char resp[2048];
    http_ctx_t ctx = { resp, 0, (int)sizeof(resp) };
    resp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 3000,
        .event_handler = http_event_handler, .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int code      = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && code == 200) {
        extract_xml(resp, "roomName", name_buf, name_sz);
        if (uuid_buf && uuid_sz) {
            char udn[64] = {0};
            if (extract_xml(resp, "UDN", udn, sizeof(udn))) {
                const char *r = strstr(udn, "RINCON");
                strncpy(uuid_buf, r ? r : udn, uuid_sz - 1);
            }
        }
    } else {
        strncpy(name_buf, ip, name_sz - 1);
    }
}

// ---- Discovery (SSDP via lwip UDP) -------------------------------------

bool sonos_controller_discover(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "Discovery starting (%lums)...", (unsigned long)timeout_ms);
    s_speaker_count = 0;
    s_active_idx    = -1;

    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(20000))) {
        ESP_LOGE(TAG, "Discovery: could not acquire network mutex");
        return false;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Discovery socket() failed: %d", errno);
        xSemaphoreGive(g_network_mutex);
        return false;
    }
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = 0 };
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    const char *msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";

    struct sockaddr_in mcast = { .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("239.255.255.250"), .sin_port = htons(1900) };
    struct sockaddr_in bcast_addr = { .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("255.255.255.255"), .sin_port = htons(1900) };

    for (int burst = 0; burst < 3; burst++) {
        sendto(sock, msearch, strlen(msearch), 0, (struct sockaddr *)&mcast, sizeof(mcast));
        sendto(sock, msearch, strlen(msearch), 0, (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
        ESP_LOGI(TAG, "SSDP burst %d/3", burst + 1);
        if (burst < 2) vTaskDelay(pdMS_TO_TICKS(500));
    }

    uint32_t start = ms_now();
    char buf[1025];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (ms_now() - start < timeout_ms) {
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        buf[len] = '\0';

        char lower[1025];
        for (int i = 0; i <= len; i++) lower[i] = (char)tolower((unsigned char)buf[i]);
        if (!strstr(lower, "sonos") && !strstr(lower, "zoneplayer")) continue;

        char ip_str[24];
        snprintf(ip_str, sizeof(ip_str), "%s", inet_ntoa(from.sin_addr));

        bool exists = false;
        for (int i = 0; i < s_speaker_count; i++)
            if (strcmp(s_speakers[i].ip, ip_str) == 0) { exists = true; break; }
        if (exists || s_speaker_count >= MAX_DEVICES) continue;

        strncpy(s_speakers[s_speaker_count].ip, ip_str, sizeof(s_speakers[s_speaker_count].ip) - 1);
        s_speakers[s_speaker_count].connected   = false;
        s_speakers[s_speaker_count].error_count = 0;
        ESP_LOGI(TAG, "Found: %s", ip_str);
        s_speaker_count++;
    }

    close(sock);
    g_last_network_end_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    xSemaphoreGive(g_network_mutex);

    ESP_LOGI(TAG, "Discovery: %d device(s) found", s_speaker_count);
    if (s_speaker_count == 0) return false;

    for (int i = 0; i < s_speaker_count; i++) {
        fetch_room_name(s_speakers[i].ip, s_speakers[i].name, sizeof(s_speakers[i].name),
                        s_speakers[i].uuid, sizeof(s_speakers[i].uuid));
        ESP_LOGI(TAG, "  [%d] %s (%s) %s", i, s_speakers[i].name, s_speakers[i].ip, s_speakers[i].uuid);
    }

    // Prefer SONOS_TARGET_SPEAKER_NAME (wifi_creds.h); fall back to the first found.
    s_active_idx = 0;
    for (int i = 0; i < s_speaker_count; i++) {
        if (strcasecmp(s_speakers[i].name, SONOS_TARGET_SPEAKER_NAME) == 0) {
            s_active_idx = i;
            break;
        }
    }

    s_speakers[s_active_idx].connected = true;
    ESP_LOGI(TAG, "Active speaker: %s (%s)", s_speakers[s_active_idx].name, s_speakers[s_active_idx].ip);
    return true;
}

// ---- Track polling ------------------------------------------------------

static void update_track_info(void)
{
    static EXT_RAM_BSS_ATTR char resp[SOAP_RESP_BUF_SZ];
    if (send_soap("AVTransport", "GetPositionInfo",
                  "<InstanceID>0</InstanceID>", resp, sizeof(resp)) != 200) return;

    char uri[256] = {0};
    static EXT_RAM_BSS_ATTR char meta_raw[2048];
    meta_raw[0] = '\0';
    extract_xml(resp, "TrackURI", uri, sizeof(uri));

    // Learn Spotify service params from a playing track's URI, e.g.
    // x-sonos-spotify:spotify%3atrack%3a...?sid=12&flags=8232&sn=5
    if (strstr(uri, "x-sonos-spotify")) {
        const char *p;
        if ((p = strstr(uri, "sid=")) != NULL) { int v = atoi(p + 4); if (v > 0) s_spotify_sid = v; }
        if ((p = strstr(uri, "sn="))  != NULL) { int v = atoi(p + 3); if (v > 0) s_spotify_sn  = v; }
    }

    extract_xml(resp, "TrackMetaData", meta_raw, sizeof(meta_raw));
    decode_html_entities(meta_raw);

    char new_title[128] = {0}, new_artist[128] = {0}, new_album[128] = {0};
    char raw_art[400] = {0}, new_art[512] = {0};
    extract_xml(meta_raw, "dc:title",        new_title,  sizeof(new_title));
    extract_xml(meta_raw, "dc:creator",      new_artist, sizeof(new_artist));
    extract_xml(meta_raw, "upnp:album",      new_album,  sizeof(new_album));
    extract_xml(meta_raw, "upnp:albumArtURI", raw_art,   sizeof(raw_art));
    if (raw_art[0] == '/')
        snprintf(new_art, sizeof(new_art), "http://%s:1400%s", s_speakers[s_active_idx].ip, raw_art);
    else
        strncpy(new_art, raw_art, sizeof(new_art) - 1);

    if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(50))) {
        strncpy(s_track.title,         new_title,  sizeof(s_track.title) - 1);
        strncpy(s_track.artist,        new_artist, sizeof(s_track.artist) - 1);
        strncpy(s_track.album,         new_album,  sizeof(s_track.album) - 1);
        strncpy(s_track.album_art_url, new_art,    sizeof(s_track.album_art_url) - 1);
        s_track.valid = true;
        xSemaphoreGive(s_track_mutex);
    }
}

static void poll_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (;;) {
        if (s_active_idx >= 0) update_track_info();
        vTaskDelay(pdMS_TO_TICKS(SONOS_POLL_INTERVAL_MS));
    }
}

// ---- Direct (server-free) Spotify playback -------------------------------

// "spotify:TYPE:ID" -> Sonos play uri + DIDL metadata. Same scheme as the
// sibling project's build_spotify_uri_meta() (sonos_controller.c:871).
static bool build_spotify_uri_meta(const char *cmd, char *uri, size_t uri_sz,
                                    char *meta, size_t meta_sz)
{
    const char *sp = strstr(cmd, "spotify:");
    if (!sp) return false;

    char type[16] = {0}, id[128] = {0};
    if (sscanf(sp, "spotify:%15[^:]:%127s", type, id) != 2) return false;

    char *q = strpbrk(id, "?&");
    if (q) *q = '\0';

    int stype = (s_spotify_sid << 8) + 7;
    char enc[192];
    snprintf(enc, sizeof(enc), "spotify%%3a%s%%3a%s", type, id);

    if (strcmp(type, "track") == 0) {
        snprintf(uri, uri_sz, "x-sonos-spotify:%s?sid=%d&flags=8224&sn=%d",
                 enc, s_spotify_sid, s_spotify_sn);
    } else if (strcmp(type, "album") == 0) {
        snprintf(uri, uri_sz, "x-rincon-cpcontainer:0004206c%s", enc);
    } else if (strcmp(type, "playlist") == 0) {
        snprintf(uri, uri_sz, "x-rincon-cpcontainer:0006206c%s", enc);
    } else {
        return false;
    }

    snprintf(meta, meta_sz,
        "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
        "<item id=\"00030020%s\" restricted=\"true\">"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">"
        "SA_RINCON%d_X_#Svc%d-0-Token</desc></item></DIDL-Lite>",
        enc, stype, stype);
    return true;
}

static bool play_spotify_direct(const char *spotify_uri)
{
    if (s_active_idx < 0) return false;
    const char *uuid = s_speakers[s_active_idx].uuid;
    if (!uuid[0]) return false;

    static EXT_RAM_BSS_ATTR char uri[256], meta[768];
    if (!build_spotify_uri_meta(spotify_uri, uri, sizeof(uri), meta, sizeof(meta)))
        return false;

    static EXT_RAM_BSS_ATTR char uri_enc[512], meta_enc[1536], args[3072], resp[512];

    send_soap("AVTransport", "RemoveAllTracksFromQueue", "<InstanceID>0</InstanceID>", resp, sizeof(resp));

    xml_encode(uri,  uri_enc,  sizeof(uri_enc));
    xml_encode(meta, meta_enc, sizeof(meta_enc));
    snprintf(args, sizeof(args),
        "<InstanceID>0</InstanceID><EnqueuedURI>%s</EnqueuedURI>"
        "<EnqueuedURIMetaData>%s</EnqueuedURIMetaData>"
        "<DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
        "<EnqueueAsNext>0</EnqueueAsNext>", uri_enc, meta_enc);
    if (send_soap("AVTransport", "AddURIToQueue", args, resp, sizeof(resp)) != 200)
        return false;

    snprintf(args, sizeof(args),
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>x-rincon-queue:%s#0</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>", uuid);
    send_soap("AVTransport", "SetAVTransportURI", args, resp, sizeof(resp));

    // Freshly-swapped queue sits before track 1 — Seek is required or Play is a no-op.
    send_soap("AVTransport", "Seek", "<InstanceID>0</InstanceID><Unit>TRACK_NR</Unit><Target>1</Target>", resp, sizeof(resp));

    int play_code = send_soap("AVTransport", "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", resp, sizeof(resp));
    ESP_LOGI(TAG, "Play spotify uri direct: %s (play=%d) %s", spotify_uri, play_code, uri);
    return true;
}

static void cmd_task(void *arg)
{
    sonos_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            play_spotify_direct(cmd.uri);
        }
    }
}

bool sonos_play_spotify_uri(const char *uri)
{
    if (!uri || !uri[0] || !s_cmd_queue) return false;
    sonos_cmd_t cmd;
    strncpy(cmd.uri, uri, sizeof(cmd.uri) - 1);
    cmd.uri[sizeof(cmd.uri) - 1] = '\0';
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

// ---- Lifecycle / accessors ------------------------------------------------

void sonos_controller_init(void)
{
    s_track_mutex = xSemaphoreCreateMutex();
    s_cmd_queue   = xQueueCreate(4, sizeof(sonos_cmd_t));
    memset(&s_track, 0, sizeof(s_track));
    ESP_LOGI(TAG, "Controller initialised");
}

void sonos_controller_start_polling(void)
{
    if (!s_poll_task) xTaskCreate(poll_task, "sonos_poll", 6144, NULL, 5, &s_poll_task);
    if (!s_cmd_task_handle) xTaskCreate(cmd_task, "sonos_cmd", 4096, NULL, 6, &s_cmd_task_handle);
}

void sonos_get_track(sonos_track_t *out)
{
    if (!out) return;
    if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(50))) {
        memcpy(out, &s_track, sizeof(*out));
        xSemaphoreGive(s_track_mutex);
    }
}

const char *sonos_active_speaker_name(void)
{
    return (s_active_idx >= 0) ? s_speakers[s_active_idx].name : "";
}

bool sonos_is_connected(void)
{
    return (s_active_idx >= 0) && s_speakers[s_active_idx].connected;
}
