#include "nfc_reader.h"
#include "board_config.h"
#include "app_config.h"

#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nfc";

#define RX_BUF_SZ 256
static uint8_t s_rxbuf[RX_BUF_SZ];
static char    s_last_uid[32] = {0};   // de-dupe: don't re-fire while the same tag sits on the reader

void nfc_reader_init(void)
{
    uart_config_t cfg = {
        .baud_rate = NFC_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(NFC_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(NFC_UART_PORT, NFC_UART_TX_GPIO, NFC_UART_RX_GPIO,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(NFC_UART_PORT, RX_BUF_SZ * 2, 0, 0, NULL, 0));
    ESP_LOGI(TAG, "UART init on port %d (TX=GPIO%d RX=GPIO%d, %d baud)",
             NFC_UART_PORT, NFC_UART_TX_GPIO, NFC_UART_RX_GPIO, NFC_UART_BAUD);
}

// Parse a decoded tag-text payload into the "service:type:id" scheme used by
// the ESP Sonos Controller project. Returns true if it looks well-formed
// (at least two colons, non-empty segments) — false means "bad tap".
static bool parse_tag_text(const char *text, char *uri_out, size_t uri_out_sz)
{
    if (!text || !text[0]) return false;

    const char *c1 = strchr(text, ':');
    const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    if (!c1 || !c2) return false;            // not "service:type:id" shaped
    if (c1 == text || c2 == c1 + 1 || !*(c2 + 1)) return false;  // empty segment

    strncpy(uri_out, text, uri_out_sz - 1);
    uri_out[uri_out_sz - 1] = '\0';
    return true;
}

// TODO: this is the unresolved piece — see the header's "OPEN QUESTION".
// Needs replacing once we know this module's actual framing. For now it
// treats the raw bytes received as if they were already the tag text
// (works only if the module happens to emit the text payload verbatim,
// which is unconfirmed).
static bool parse_frame(const uint8_t *data, size_t len, char *text_out, size_t text_out_sz)
{
    if (len == 0) return false;
    size_t n = (len < text_out_sz - 1) ? len : text_out_sz - 1;
    memcpy(text_out, data, n);
    text_out[n] = '\0';
    return true;
}

nfc_tap_result_t nfc_reader_poll(char *uri_out, size_t uri_out_sz)
{
    int len = uart_read_bytes(NFC_UART_PORT, s_rxbuf, sizeof(s_rxbuf) - 1,
                              pdMS_TO_TICKS(20));
    if (len <= 0) return NFC_TAP_NONE;
    s_rxbuf[len] = '\0';

    char text[128] = {0};
    if (!parse_frame(s_rxbuf, (size_t)len, text, sizeof(text))) {
        ESP_LOGW(TAG, "tap: unparseable frame (%d bytes)", len);
        return NFC_TAP_BAD;
    }

    // De-dupe: ignore repeats of the same payload while the tag is still
    // resting on the reader (no explicit "tag removed" signal from this
    // module yet — revisit once the real protocol is known).
    if (strcmp(text, s_last_uid) == 0) return NFC_TAP_NONE;
    strncpy(s_last_uid, text, sizeof(s_last_uid) - 1);

    if (!parse_tag_text(text, uri_out, uri_out_sz)) {
        ESP_LOGW(TAG, "tap: bad tag text \"%s\"", text);
        return NFC_TAP_BAD;
    }

    ESP_LOGI(TAG, "tap: %s", uri_out);
    return NFC_TAP_GOOD;
}
