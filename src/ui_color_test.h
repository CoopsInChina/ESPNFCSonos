#pragma once

// Diagnostic-only screen: 8 labeled solid-color swatches (pure red, green,
// blue, white, black, yellow, cyan, magenta), drawn through lv_color_make()
// — the exact same path album art pixels go through. Lets us isolate a
// display color problem (BGR order, inversion) from JPEG decode/network
// entirely, with a photo takeable seconds after flashing.
void ui_color_test_show(void);

// Synthetic gradient pushed through the exact same raw-buffer
// lv_img_dsc_t + lv_img_set_src() path album_art.c uses, with no JPEG
// decoding involved — isolates whether a display artifact is in that
// rendering path itself vs. the JPEG decoder.
void ui_color_test_show_gradient(void);
