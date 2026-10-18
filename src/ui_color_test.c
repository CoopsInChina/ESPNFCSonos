#include "ui_color_test.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <string.h>

typedef struct { const char *name; uint32_t hex; } swatch_t;

// If a swatch's label says "RED" but it visibly displays as a different
// hue (e.g. blue), that's a channel-order (RGB/BGR) problem. If the whole
// set looks like its own negative (WHITE looks black, BLACK looks white,
// RED looks cyan), that's a color-inversion problem.
static const swatch_t s_swatches[8] = {
    { "RED",     0xFF0000 },
    { "GREEN",   0x00FF00 },
    { "BLUE",    0x0000FF },
    { "WHITE",   0xFFFFFF },
    { "BLACK",   0x000000 },
    { "YELLOW",  0xFFFF00 },
    { "CYAN",    0x00FFFF },
    { "MAGENTA", 0xFF00FF },
};

// Synthetic raw-buffer test image: a smooth diagonal gradient, generated
// in code (no JPEG involved at all) and pushed through the exact same
// lv_img_dsc_t + lv_img_set_src() path album_art.c uses. If THIS shows
// static too, the bug is in that raw-buffer rendering path itself
// (buffer alloc/cache/stride), not in JPEG decoding — isolates the two
// completely.
static void show_gradient_test(lv_obj_t *parent)
{
    static lv_img_dsc_t dsc;
    uint16_t *buf = (uint16_t *)heap_caps_malloc(200 * 200 * sizeof(uint16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;

    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            uint8_t r = (uint8_t)(x * 255 / 199);
            uint8_t g = (uint8_t)(y * 255 / 199);
            uint8_t b = (uint8_t)((x + y) * 255 / 398);
            buf[y * 200 + x] = (uint16_t)lv_color_make(r, g, b).full;
        }
    }

    memset(&dsc, 0, sizeof(dsc));
    dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc.header.w  = 200;
    dsc.header.h  = 200;
    dsc.data_size = 200 * 200 * sizeof(uint16_t);
    dsc.data      = (const uint8_t *)buf;

    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &dsc);
    lv_obj_set_pos(img, 80, 80);
}

void ui_color_test_show(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x808080), 0);   // neutral gray backdrop
    lv_obj_set_style_pad_all(scr, 0, 0);

    const int cols = 4, rows = 2;
    const int box = 70, gap = 6;
    const int grid_w = cols * box + (cols - 1) * gap;
    const int grid_h = rows * (box + 18) + (rows - 1) * gap;
    const int x0 = (360 - grid_w) / 2;
    const int y0 = (360 - grid_h) / 2;

    for (int i = 0; i < 8; i++) {
        int col = i % cols, row = i / cols;
        int x = x0 + col * (box + gap);
        int y = y0 + row * (box + 18 + gap);

        lv_obj_t *sw = lv_obj_create(scr);
        lv_obj_set_size(sw, box, box);
        lv_obj_set_pos(sw, x, y);
        lv_obj_set_style_bg_color(sw, lv_color_hex(s_swatches[i].hex), 0);
        lv_obj_set_style_border_width(sw, 2, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(0x000000), 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, s_swatches[i].name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lbl, x, y + box + 2);
    }

    lv_scr_load(scr);
}

void ui_color_test_show_gradient(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x808080), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    show_gradient_test(scr);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "GRADIENT (raw buffer, no JPEG)");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_scr_load(scr);
}
