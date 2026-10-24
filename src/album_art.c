/**
 * Fullscreen album art — simplified port of
 * /Users/ESP32MusicAndMeat/src/ui_art.c (itself ported from ESPSonos
 * ui_album_art.cpp). Dropped the multi-slot favourites blob cache (not
 * needed here — this device only ever shows "now playing").
 */

#include "album_art.h"
#include "globals.h"
#include "app_config.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/stats.h"   // TCP_STATS_DISPLAY() — see CONFIG_LWIP_STATS note below
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
// TJpgDec is bundled inside LVGL 8.3.x (extra/libs/sjpg) — see idf_component.yml.
#include "extra/libs/sjpg/tjpgd.h"
#include <string.h>

static const char *TAG = "album_art";

#define ART_W DISPLAY_WIDTH
#define ART_H DISPLAY_HEIGHT
#define ART_PIXEL_BUF_BYTES  (ART_W * ART_H * sizeof(lv_color_t))
#define ART_TJPGD_POOL_BYTES 4096
#define ART_TASK_STACK_SZ    ART_TASK_STACK
#define ART_CONSUMED_TIMEOUT_MS 6000

static uint16_t    *s_disp_buf = NULL;
static uint16_t    *s_work_buf = NULL;
static lv_img_dsc_t s_art_dsc;
static volatile bool s_new_art = false;
static volatile bool s_loading = false;

static SemaphoreHandle_t s_url_mutex = NULL;
static EXT_RAM_BSS_ATTR char s_pending[512] = {0};
static EXT_RAM_BSS_ATTR char s_current[512] = {0};

static uint8_t s_tjpgd_pool[ART_TJPGD_POOL_BYTES] __attribute__((aligned(4)));

typedef struct {
    const uint8_t *data; size_t len, pos;
    lv_color_t    *buf;  int img_w, img_h;
} jpg_ctx_t;

static size_t jpg_infunc(JDEC *jd, uint8_t *dst, size_t nbytes)
{
    jpg_ctx_t *ctx = (jpg_ctx_t *)jd->device;
    size_t avail = ctx->len - ctx->pos;
    if (!avail) return 0;
    size_t n = (nbytes < avail) ? nbytes : avail;
    if (dst) memcpy(dst, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return n;
}

static int jpg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_ctx_t     *ctx = (jpg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int blk_w = rect->right - rect->left + 1;
    int blk_h = rect->bottom - rect->top + 1;
    // TJpgDec's mcu_output() always builds `bitmap` at the FULL MCU size
    // (jd->msx*8 x jd->msy*8 — e.g. 16x16 for standard 4:2:0 chroma
    // subsampling), but for a block clipped at the image's right/bottom
    // edge (width/height not an exact MCU multiple), `rect` reports the
    // smaller CLIPPED size. Using blk_w as the read stride only happens
    // to be correct for non-edge blocks; for edge blocks it reads the
    // wrong bytes for every row past the first, scrambling pixels. The
    // real stride is always the full MCU width.
    int stride = jd->msx * 8;
    for (int y = 0; y < blk_h; y++) {
        int oy = rect->top + y;
        if (oy >= ctx->img_h) continue;
        for (int x = 0; x < blk_w; x++) {
            int ox = rect->left + x;
            if (ox >= ctx->img_w) continue;
            const uint8_t *p = src + (y * stride + x) * 3;
            ctx->buf[oy * ctx->img_w + ox] = lv_color_make(p[0], p[1], p[2]);
        }
    }
    return 1;
}

static lv_color_t *decode_jpeg(const uint8_t *data, size_t len, int *out_w, int *out_h)
{
    jpg_ctx_t ctx = { .data = data, .len = len, .pos = 0, .buf = NULL, .img_w = 0, .img_h = 0 };
    JDEC jd;
    if (jd_prepare(&jd, jpg_infunc, s_tjpgd_pool, sizeof(s_tjpgd_pool), &ctx) != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare failed");
        return NULL;
    }
    int w = (int)jd.width, h = (int)jd.height;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        ESP_LOGW(TAG, "JPEG dims invalid: %dx%d", w, h);
        return NULL;
    }
    size_t buf_bytes = (size_t)w * h * sizeof(lv_color_t);
    lv_color_t *buf = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "decode buf alloc failed (%u B)", (unsigned)buf_bytes); return NULL; }

    ctx.buf = buf; ctx.img_w = w; ctx.img_h = h;
    if (jd_decomp(&jd, jpg_outfunc, 0) != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp failed");
        heap_caps_free(buf);
        return NULL;
    }
    *out_w = w; *out_h = h;
    return buf;
}

// Nearest-neighbor scaling. A bilinear version was tried first but produced
// scattered speckle corruption on every real photo — confirmed (by this
// vs. that) to come specifically from reading the second interpolation
// neighbor (x1/y1, a row elsewhere in PSRAM), not from JPEG decode, the
// display pipeline, or color settings, all of which were separately ruled
// out first. Likely a PSRAM access-pattern sensitivity to alternating
// reads between two widely-separated rows rather than a plain logic bug.
// Not worth chasing further for a marginal quality gain on a small round
// thumbnail — this renders cleanly.
static void scale_nearest(const uint16_t *src, int src_w, int src_h, int src_stride,
                           uint16_t *dst, int dst_w, int dst_h)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * src_h / dst_h;
        if (sy >= src_h) sy = src_h - 1;
        const uint16_t *srow = src + sy * src_stride;
        uint16_t *drow = dst + dy * dst_w;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = dx * src_w / dst_w;
            if (sx >= src_w) sx = src_w - 1;
            drow[dx] = srow[sx];
        }
    }
}

// Minimal URL fixups needed for Sonos/Spotify album art specifically
// (dropped the sibling project's TuneIn/Deezer radio-CDN quirks — not
// relevant, this device only plays Spotify via NFC taps).
static void prepare_url(const char *raw, char *out, size_t sz)
{
    strncpy(out, raw, sz - 1);
    out[sz - 1] = '\0';

    // Sonos DIDL single-slash quirk: https:/ -> https://
    if (!strncmp(out, "https:/", 7) && out[7] != '/') {
        size_t len = strlen(out);
        if (len + 1 < sz) { memmove(out + 8, out + 7, len - 7 + 1); out[7] = '/'; }
    }
    // HTTPS -> HTTP for the local Sonos speaker (:1400/) — avoids TLS entirely for local art.
    if (!strncmp(out, "https://", 8) && strstr(out, ":1400/"))
        memmove(out + 4, out + 5, strlen(out + 5) + 1);

    // Sonos getaa: request art pre-sized to the display, far less data than full-res.
    if (strstr(out, "/getaa?") && !strstr(out, "maxWidth")) {
        size_t cur = strlen(out);
        char suffix[40];
        snprintf(suffix, sizeof(suffix), "&maxWidth=%d&maxHeight=%d", ART_W, ART_H);
        if (cur + strlen(suffix) < sz) strncat(out, suffix, sz - cur - 1);
    }

    // Spotify CDN direct link (Sonos hands these out as-is for Spotify
    // tracks — no /getaa proxy involved, so the maxWidth/maxHeight trick
    // above never applies). Spotify's image ID encodes a fixed size as a
    // constant 8-hex-char segment right after the "ab67616d" prefix —
    // swapping it for another of the CDN's own fixed sizes gets a real,
    // much smaller image instead of downloading 640x640 and scaling it
    // down ourselves. 300x300 is the closest fixed size above our
    // DISPLAY_WIDTH/HEIGHT (360x360); the only other option is 64x64,
    // too small to upscale cleanly.
    char *size_code = strstr(out, "ab67616d0000b273");
    if (size_code) memcpy(size_code + 8, "00001e02", 8);
}

typedef struct { uint8_t *buf; size_t max; size_t total; bool overflowed; int64_t t0; } dl_ctx_t;

// DIAGNOSTIC: logs every chunk's arrival time + gap since the previous one,
// to see the actual SHAPE of a slow download — steady-but-slow throughout
// (points at crypto/PHY throughput) vs. fast bursts separated by multi-
// second gaps (points at flow control / scheduling / buffer stalls).
// These are different bugs with different fixes; we can't tell which
// without this.
static esp_err_t art_dl_evt(esp_http_client_event_t *evt)
{
    dl_ctx_t *ctx = (dl_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        ESP_LOGI(TAG, "  [t=%lldms] CONNECTED", (esp_timer_get_time() - ctx->t0) / 1000);
    } else if (evt->event_id == HTTP_EVENT_REDIRECT) {
        ctx->total = 0;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int64_t now = esp_timer_get_time();
        static int64_t s_last_chunk_t;
        int64_t gap_ms = ctx->total == 0 ? 0 : (now - s_last_chunk_t) / 1000;
        s_last_chunk_t = now;
        ESP_LOGI(TAG, "  [t=%lldms] +%d bytes (gap=%lldms, total=%u)",
                 (now - ctx->t0) / 1000, evt->data_len, gap_ms, (unsigned)ctx->total);

        if (ctx->total + (size_t)evt->data_len > ctx->max) {
            // Oversized response — abort now instead of silently discarding
            // data for the rest of a potentially very long, slow transfer
            // (this is what previously turned a too-big image into a 60s
            // stall holding the shared network mutex the whole time).
            ctx->overflowed = true;
            return ESP_FAIL;
        }
        size_t copy = (size_t)evt->data_len;
        memcpy(ctx->buf + ctx->total, evt->data, copy);
        ctx->total += copy;
    }
    return ESP_OK;
}

static size_t download_url(const char *url, uint8_t *buf, size_t buf_sz)
{
    dl_ctx_t ctx = { .buf = buf, .max = buf_sz - 1, .total = 0, .overflowed = false, .t0 = esp_timer_get_time() };
    esp_http_client_config_t cfg = {
        // Matches the sibling Grill & Groove project's proven config
        // exactly (same value they use for art downloads on the same
        // chip) — reverted from a speculative 16384 bump that wasn't
        // actually based on their working reference.
        .url = url, .timeout_ms = 8000, .buffer_size = 4096,
        .max_redirection_count = 3,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .event_handler = art_dl_evt, .user_data = &ctx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;
    esp_http_client_set_header(c, "Accept", "image/jpeg, image/png");

    int64_t t0 = esp_timer_get_time();
    esp_err_t err    = esp_http_client_perform(c);
    int       status = esp_http_client_get_status_code(c);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    esp_http_client_cleanup(c);

    if (ctx.overflowed) {
        ESP_LOGW(TAG, "download aborted: exceeds %uB buffer (elapsed=%lldms before abort)",
                 (unsigned)ctx.max, elapsed_ms);
        return 0;
    }
    ESP_LOGI(TAG, "download: err=%d status=%d bytes=%u elapsed=%lldms",
             (int)err, status, (unsigned)ctx.total, elapsed_ms);

    if (err != ESP_OK || status != 200) {
        return 0;
    }
    return ctx.total;
}

static void art_task(void *arg)
{
    uint8_t *dl_buf = (uint8_t *)heap_caps_malloc(ART_MAX_DOWNLOAD_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dl_buf) { ESP_LOGE(TAG, "dl_buf alloc failed — art disabled"); vTaskDelete(NULL); return; }

    static EXT_RAM_BSS_ATTR char url[512];

    for (;;) {
        bool has_work;
        xSemaphoreTake(s_url_mutex, portMAX_DELAY);
        has_work = (s_pending[0] != '\0' && strcmp(s_pending, s_current) != 0);
        if (has_work) {
            prepare_url(s_pending, url, sizeof(url));
            strncpy(s_current, s_pending, sizeof(s_current) - 1);
            s_loading = true;
        }
        xSemaphoreGive(s_url_mutex);

        if (!has_work) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }

        ESP_LOGI(TAG, "requesting: %s", url);
        size_t dl_len = 0;
        if (xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
            net_pre_wait("ART");
            dl_len = download_url(url, dl_buf, ART_MAX_DOWNLOAD_BYTES);
            g_last_network_end_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            xSemaphoreGive(g_network_mutex);
            // DIAGNOSTIC: cumulative-since-boot TCP stats. A high rexmit/drop
            // count confirms packet loss is the real bottleneck (not a
            // config/throughput ceiling) — the per-chunk timing log showed
            // bursty transfers with multi-second gaps that double each time,
            // the textbook sign of TCP retransmit timeout backoff.
            TCP_STATS_DISPLAY();
        }
        if (dl_len < 3 || dl_buf[0] != 0xFF || dl_buf[1] != 0xD8) {
            ESP_LOGW(TAG, "download failed or not JPEG (%u bytes, first: %02x %02x %02x %02x)",
                     (unsigned)dl_len, dl_buf[0], dl_buf[1], dl_buf[2], dl_buf[3]);
            xSemaphoreTake(s_url_mutex, portMAX_DELAY);
            s_current[0] = '\0';
            xSemaphoreGive(s_url_mutex);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int iw = 0, ih = 0;
        lv_color_t *decoded = decode_jpeg(dl_buf, dl_len, &iw, &ih);
        if (!decoded) continue;

        scale_nearest((const uint16_t *)decoded, iw, ih, iw, s_work_buf, ART_W, ART_H);
        heap_caps_free(decoded);
        ESP_LOGI(TAG, "scaled %dx%d -> %dx%d", iw, ih, ART_W, ART_H);

        uint16_t *tmp = s_disp_buf; s_disp_buf = s_work_buf; s_work_buf = tmp;
        s_new_art = true;
        uint32_t t0 = xTaskGetTickCount();
        while (s_new_art && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(ART_CONSUMED_TIMEOUT_MS))
            vTaskDelay(pdMS_TO_TICKS(100));
        s_new_art = false;
    }
}

void album_art_init(void)
{
    s_url_mutex = xSemaphoreCreateMutex();
    s_disp_buf = (uint16_t *)heap_caps_calloc(ART_PIXEL_BUF_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_work_buf = (uint16_t *)heap_caps_calloc(ART_PIXEL_BUF_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_disp_buf || !s_work_buf) ESP_LOGE(TAG, "art buf alloc failed");

    memset(&s_art_dsc, 0, sizeof(s_art_dsc));
    s_art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_art_dsc.header.w  = ART_W;
    s_art_dsc.header.h  = ART_H;
    s_art_dsc.data_size = ART_PIXEL_BUF_BYTES;
    s_art_dsc.data      = (const uint8_t *)s_disp_buf;

    // Priority bumped from 2 (lowest in the system) to above everything
    // else: TCP stats confirmed near-zero packet loss despite the bursty
    // arrival pattern, meaning data was sitting ready in the kernel's
    // receive buffer while this task simply wasn't scheduled promptly
    // enough to drain it — not a network problem. It mostly blocks on
    // network I/O anyway (yielding naturally during real waits), so
    // running it above LVGL/polling shouldn't starve them, just stop it
    // from being starved by them.
    xTaskCreatePinnedToCore(art_task, "album_art", ART_TASK_STACK_SZ, NULL, 10, NULL, 1);
    ESP_LOGI(TAG, "init OK");
}

void album_art_request(const char *raw_url)
{
    if (!s_url_mutex) return;
    xSemaphoreTake(s_url_mutex, portMAX_DELAY);
    if (raw_url && raw_url[0]) strncpy(s_pending, raw_url, sizeof(s_pending) - 1);
    else s_pending[0] = '\0';
    xSemaphoreGive(s_url_mutex);
}

bool album_art_update(lv_obj_t *img_obj)
{
    if (!s_new_art || !img_obj) return false;
    s_new_art = false;
    s_loading = false;
    s_art_dsc.data = (const uint8_t *)s_disp_buf;
    lv_img_set_src(img_obj, &s_art_dsc);
    return true;
}

bool album_art_is_loading(void)
{
    return s_loading;
}
