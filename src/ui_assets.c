#include "ui_assets.h"
#include "app_config.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
// TJpgDec is bundled inside LVGL 8.3.x (extra/libs/sjpg) — see idf_component.yml.
#include "extra/libs/sjpg/tjpgd.h"
#include <string.h>

// Generated C byte arrays (see src/assets/*.h) — not idf_component_
// register's EMBED_FILES, which mangles its intermediate .S-file paths
// under PlatformIO's ESP-IDF integration. Regenerate these from the
// original .jpg files with the script in README.md's "Asset pipeline"
// section if the source images ever change.
#include "assets/boot_jpg.h"
#include "assets/wifi_jpg.h"
#include "assets/wifispeaker_jpg.h"
#include "assets/ready_jpg.h"
#include "assets/setup_jpg.h"

static const char *TAG = "ui_assets";

#define ASSET_W DISPLAY_WIDTH
#define ASSET_H DISPLAY_HEIGHT
#define ASSET_TJPGD_POOL_BYTES 4096

static uint8_t s_tjpgd_pool[ASSET_TJPGD_POOL_BYTES] __attribute__((aligned(4)));

static lv_img_dsc_t s_boot_dsc, s_wifi_dsc, s_wifispeaker_dsc, s_ready_dsc, s_setup_dsc;

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

// Same stride fix as album_art.c's jpg_outfunc — the true MCU stride is
// jd->msx*8, not the clipped block width, or edge blocks scramble.
static int jpg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_ctx_t     *ctx = (jpg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int blk_w = rect->right - rect->left + 1;
    int blk_h = rect->bottom - rect->top + 1;
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

// Decodes straight into an lv_img_dsc_t backed by a PSRAM buffer — no
// scaling needed, the source JPEGs were pre-sized to ASSET_W x ASSET_H.
static bool decode_asset(const char *name, const uint8_t *data, size_t len, lv_img_dsc_t *out_dsc)
{
    jpg_ctx_t ctx = { .data = data, .len = len, .pos = 0, .buf = NULL, .img_w = 0, .img_h = 0 };
    JDEC jd;
    if (jd_prepare(&jd, jpg_infunc, s_tjpgd_pool, sizeof(s_tjpgd_pool), &ctx) != JDR_OK) {
        ESP_LOGE(TAG, "%s: jd_prepare failed", name);
        return false;
    }
    int w = (int)jd.width, h = (int)jd.height;
    if (w != ASSET_W || h != ASSET_H) {
        ESP_LOGW(TAG, "%s is %dx%d, expected %dx%d — displaying anyway", name, w, h, ASSET_W, ASSET_H);
    }

    size_t buf_bytes = (size_t)w * h * sizeof(lv_color_t);
    lv_color_t *buf = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "%s: alloc failed (%u B)", name, (unsigned)buf_bytes); return false; }

    ctx.buf = buf; ctx.img_w = w; ctx.img_h = h;
    if (jd_decomp(&jd, jpg_outfunc, 0) != JDR_OK) {
        ESP_LOGE(TAG, "%s: jd_decomp failed", name);
        heap_caps_free(buf);
        return false;
    }

    memset(out_dsc, 0, sizeof(*out_dsc));
    out_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    out_dsc->header.w  = w;
    out_dsc->header.h  = h;
    out_dsc->data_size = (uint32_t)buf_bytes;
    out_dsc->data      = (const uint8_t *)buf;
    return true;
}

void ui_assets_init(void)
{
    decode_asset("boot",        boot_jpg_data,        boot_jpg_len,        &s_boot_dsc);
    decode_asset("wifi",        wifi_jpg_data,        wifi_jpg_len,        &s_wifi_dsc);
    decode_asset("wifispeaker", wifispeaker_jpg_data, wifispeaker_jpg_len, &s_wifispeaker_dsc);
    decode_asset("ready",       ready_jpg_data,       ready_jpg_len,       &s_ready_dsc);
    decode_asset("setup",       setup_jpg_data,       setup_jpg_len,       &s_setup_dsc);
    ESP_LOGI(TAG, "init OK");
}

const lv_img_dsc_t *ui_asset_boot(void)        { return &s_boot_dsc; }
const lv_img_dsc_t *ui_asset_wifi(void)        { return &s_wifi_dsc; }
const lv_img_dsc_t *ui_asset_wifispeaker(void) { return &s_wifispeaker_dsc; }
const lv_img_dsc_t *ui_asset_ready(void)       { return &s_ready_dsc; }
const lv_img_dsc_t *ui_asset_setup(void)       { return &s_setup_dsc; }
