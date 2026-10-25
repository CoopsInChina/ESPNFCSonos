#include "nfc_reader.h"
#include "board_config.h"
#include "app_config.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "nfc";

// Protocol confirmed against a real, working Arduino library for this exact
// module (github.com/TCL47-GITHUB/RC522_UART) — a host-driven
// command/response scheme, framed as HEADER cmd [data...] TAIL, where TAIL
// is TAIL_OK on success or TAIL_ERR on failure. Nothing is auto-reported;
// every read starts with us sending a command.
#define HEADER    0x7F
#define TAIL_OK   0xF7
#define TAIL_ERR  0x55

#define CMD_RESET     0x00
#define CMD_ANT_ON    0x01
#define CMD_ANT_OFF   0x02
#define CMD_REQUEST   0x03
#define CMD_ANTICOLL  0x04
#define CMD_SELECT    0x05
#define CMD_AUTH      0x06
#define CMD_READ      0x07
#define CMD_WRITE     0x08
#define CMD_HALT      0x0B

#define REQ_ALL       0x52
#define AUTH_KEY_A    0x60

// Same candidate keys, same sector/block layout as the sibling ESP Sonos
// Controller project's Mifare Classic NDEF reader
// (/Users/garry.cooper/Sonos/src/pn532_nfc_manager.cpp) — these tags were
// written by that project, so reading has to match it exactly.
static const uint8_t CANDIDATE_KEYS[3][6] = {
    { 0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7 },   // NFC Forum NDEF key
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },   // Factory default
    { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 },   // MAD key (sector 0)
};

static char         s_last_uid_hex[16] = {0};   // de-dupe: don't re-fire while the same tag sits on the reader
static volatile bool s_healthy = false;         // ANT_ON confirmed acknowledged — see nfc_reader_is_healthy()

// Consecutive missed REQUESTs required before treating the card as
// actually removed (clearing s_last_uid_hex) — a single miss (noise, brief
// field dropout, a bit of card wobble) was clearing it immediately, which
// at the old 300ms poll interval was rare enough not to matter, but at the
// current faster interval started reliably producing a second "tap" (and
// a second play) for a card that never actually left the reader.
#define UID_ABSENCE_CONFIRM_MISSES 3
static int s_absence_misses = 0;

// Logging every byte of every exchange clogs the log on a link that's
// mostly just idling (REQUEST/no-card every poll). What's actually
// useful is knowing when the link's *behavior* changes — log only on
// that transition, not on every poll that confirms nothing changed.
typedef enum { NFC_LINK_UNKNOWN, NFC_LINK_NORMAL, NFC_LINK_ABNORMAL, NFC_LINK_ERROR } nfc_link_state_t;
static nfc_link_state_t s_link_state = NFC_LINK_UNKNOWN;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void flush_rx(void)
{
    uint8_t tmp[32];
    while (uart_read_bytes(NFC_UART_PORT, tmp, sizeof(tmp), 0) > 0) { }
}

static void log_hex(const char *label, const uint8_t *data, size_t len)
{
    char hex[3 * 32];
    size_t hpos = 0;
    for (size_t i = 0; i < len && hpos + 3 < sizeof(hex); i++)
        hpos += snprintf(hex + hpos, sizeof(hex) - hpos, "%02X ", data[i]);
    ESP_LOGI(TAG, "%s (%u bytes): %s", label, (unsigned)len, hex);
}

static void set_link_state(nfc_link_state_t state)
{
    if (state == s_link_state) return;
    s_link_state = state;
    static const char *names[] = { "Unknown", "Normal", "Abnormal", "Error" };
    ESP_LOGW(TAG, "NFC State: %s", names[state]);
}

// On the first poll that goes abnormal, also log the actual bytes that
// triggered it — useful context exactly once, not on every poll while
// it stays abnormal.
static void note_abnormal(const uint8_t *data, size_t len)
{
    bool first = (s_link_state != NFC_LINK_ABNORMAL);
    set_link_state(NFC_LINK_ABNORMAL);
    if (first) log_hex("  first abnormal bytes", data, len);
}

// Bytes of the last frame we transmitted — some firmware echoes the
// just-sent frame back before its real reply, for any command, not just
// REQUEST. recv_packet() compares against this to recognize and skip that
// echo regardless of which command triggered it.
static uint8_t s_last_tx[32];
static size_t  s_last_tx_len = 0;

static void send_packet(uint8_t cmd, const uint8_t *data, size_t len)
{
    flush_rx();
    uint8_t pkt[32];
    size_t  n = 0;
    pkt[n++] = HEADER;
    pkt[n++] = cmd;
    if (data && len) { memcpy(pkt + n, data, len); n += len; }
    pkt[n++] = TAIL_OK;
    uart_write_bytes(NFC_UART_PORT, (const char *)pkt, n);
    memcpy(s_last_tx, pkt, n);
    s_last_tx_len = n;
}

// Reads a HEADER [data (expected_len bytes)] TAIL reply, resyncing on noise
// exactly like the reference library does. Returns false on TAIL_ERR or
// timeout. A clean TAIL_OK or TAIL_ERR both count as NFC_LINK_NORMAL —
// they're both well-formed replies (ERR just means "no card"/"not ok",
// not a link problem); resyncing on noise is NFC_LINK_ABNORMAL; a hard
// timeout is NFC_LINK_ERROR.
static bool recv_packet(uint8_t *out, size_t expected_len, uint32_t timeout_ms)
{
    uint8_t buf[32];
    size_t  n = 0;
    size_t  total = expected_len + 2;   // + header + tail
    uint32_t start = now_ms();

    while (now_ms() - start < timeout_ms) {
        uint8_t b;
        int got = uart_read_bytes(NFC_UART_PORT, &b, 1, pdMS_TO_TICKS(10));
        if (got <= 0) continue;
        if (n < sizeof(buf)) buf[n++] = b;

        // Some firmwares echo back exactly the frame we just sent, before
        // the real reply — seen on REQUEST, but not guaranteed to be
        // limited to it (the periodic ANT_ON reassert in
        // reassert_antenna_if_due() sends a different, shorter frame).
        // While what we have so far could still be a prefix of that echo,
        // keep accumulating instead of falling into the n >= total check
        // below — otherwise a short reply (like ANT_ON's 2-byte ack) gets
        // judged, and misclassified as corrupt, before a longer echo even
        // finishes arriving, which would make the periodic antenna
        // reassert spuriously fail and flip nfc_reader_is_healthy() to
        // false. Once the full echo has matched, discard it and keep
        // waiting for the real reply; any mismatch falls through below.
        if (s_last_tx_len && n <= s_last_tx_len && memcmp(buf, s_last_tx, n) == 0) {
            if (n == s_last_tx_len) n = 0;
            continue;
        }

        if (n >= total) {
            if (buf[0] != HEADER) {
                note_abnormal(buf, n);
                memmove(buf, buf + 1, --n);
                continue;
            }
            uint8_t tail = buf[n - 1];

            if (tail == TAIL_OK) {
                set_link_state(NFC_LINK_NORMAL);
                if (out && expected_len) memcpy(out, buf + 1, expected_len);
                return true;
            } else if (tail == TAIL_ERR) {
                set_link_state(NFC_LINK_NORMAL);
                return false;
            } else {
                note_abnormal(buf, n);
                memmove(buf, buf + 1, --n);
            }
        }
    }
    set_link_state(NFC_LINK_ERROR);
    return false;
}

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
    ESP_ERROR_CHECK(uart_driver_install(NFC_UART_PORT, 256, 0, 0, NULL, 0));
    ESP_LOGI(TAG, "UART init on port %d (TX=GPIO%d RX=GPIO%d, %d baud)",
             NFC_UART_PORT, NFC_UART_TX_GPIO, NFC_UART_RX_GPIO, NFC_UART_BAUD);

    // "Enter normal mode" — a bare, unframed 0x01 answered with a bare 0xAA.
    // Distinct from the framed CMD_ANT_ON (which also happens to be 0x01).
    uint8_t enter_normal = 0x01;
    uart_write_bytes(NFC_UART_PORT, (const char *)&enter_normal, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t reply = 0;
    int got_bytes = uart_read_bytes(NFC_UART_PORT, &reply, 1, pdMS_TO_TICKS(50));
    bool got_aa = (got_bytes == 1 && reply == 0xAA);
    ESP_LOGI(TAG, "enter-normal-mode: %s", got_aa ? "ACK (0xAA)" : "no/other reply, trying reset");
    flush_rx();

    if (!got_aa) {
        send_packet(CMD_RESET, NULL, 0);
        bool reset_ok = recv_packet(NULL, 0, 200);
        ESP_LOGI(TAG, "reset: %s", reset_ok ? "ok" : "no reply");
    }

    send_packet(CMD_ANT_ON, NULL, 0);
    bool ant_ok = recv_packet(NULL, 0, 200);
    ESP_LOGI(TAG, "antenna on: %s", ant_ok ? "ok" : "no reply");
    // Healthy means the link answered at all, not specifically "ant_ok":
    // see reassert_antenna_if_due() for why a well-formed TAIL_ERR isn't
    // a health problem.
    s_healthy = (s_link_state == NFC_LINK_NORMAL);
}

static bool select_uid(const uint8_t *uid)
{
    send_packet(CMD_SELECT, uid, 4);
    return recv_packet(NULL, 0, 200);
}

static bool auth_block(uint8_t block_addr, const uint8_t *key, const uint8_t *uid)
{
    uint8_t data[12];
    data[0] = AUTH_KEY_A;
    data[1] = block_addr;
    memcpy(data + 2, key, 6);
    memcpy(data + 8, uid, 4);
    send_packet(CMD_AUTH, data, sizeof(data));
    return recv_packet(NULL, 0, 300);
}

static bool read_block(uint8_t block_addr, uint8_t *out16)
{
    send_packet(CMD_READ, &block_addr, 1);
    return recv_packet(out16, 16, 200);
}

// Validate/copy a decoded NDEF text payload into the "service:type:id"
// scheme used by the ESP Sonos Controller project (e.g.
// "spotify:album:xxx") — two colons, non-empty segments.
static bool parse_tag_text(const char *text, char *uri_out, size_t uri_out_sz)
{
    if (!text || !text[0]) return false;
    const char *c1 = strchr(text, ':');
    const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    if (!c1 || !c2) return false;
    if (c1 == text || c2 == c1 + 1 || !*(c2 + 1)) return false;
    strncpy(uri_out, text, uri_out_sz - 1);
    uri_out[uri_out_sz - 1] = '\0';
    return true;
}

// Read an NDEF Text record from a Mifare Classic 1K tag (4-byte UID,
// SAK=0x08). Ported 1:1 from the sibling ESP Sonos Controller project's
// readNDEFTextMifareClassic() — same key probing, same sector/block
// layout, same TLV/NDEF parsing — just over this module's UART commands
// instead of PN532 I2C/SPI calls.
static bool read_ndef_text(const uint8_t *uid, char *text_out, size_t text_out_sz)
{
    const uint8_t *working_key = NULL;
    for (int k = 0; k < 3; k++) {
        if (auth_block(4, CANDIDATE_KEYS[k], uid)) {
            working_key = CANDIDATE_KEYS[k];
            ESP_LOGI(TAG, "Mifare Classic: sector 1 auth OK with key[%d]", k);
            break;
        }
    }
    if (!working_key) {
        ESP_LOGW(TAG, "Mifare Classic: sector 1 auth failed with all keys");
        return false;
    }

    // Sectors 1-3 = blocks 4-15, 3 data blocks of 16 bytes per sector
    // (block offset 3 of each sector is the trailer — skipped).
    uint8_t all_data[144];
    int     data_len = 0;
    for (int sector = 1; sector <= 3 && data_len < (int)sizeof(all_data); sector++) {
        int first_block = sector * 4;
        if (!auth_block(first_block, working_key, uid)) break;
        for (int blk = 0; blk < 3; blk++) {
            uint8_t buf[16];
            if (!read_block(first_block + blk, buf)) {
                ESP_LOGW(TAG, "Mifare Classic: read failed at block %d", first_block + blk);
                break;
            }
            memcpy(all_data + data_len, buf, 16);
            data_len += 16;
        }
    }
    if (data_len < 2) return false;

    // TLV stream starting at byte 0 of the first data block.
    int i = 0;
    while (i < data_len) {
        uint8_t tlv_type = all_data[i++];
        if (tlv_type == 0xFE) break;
        if (tlv_type == 0x00) continue;

        if (i >= data_len) break;
        uint16_t tlv_len;
        if (all_data[i] == 0xFF) {
            i++;
            if (i + 2 > data_len) break;
            tlv_len = ((uint16_t)all_data[i] << 8) | all_data[i + 1];
            i += 2;
        } else {
            tlv_len = all_data[i++];
        }

        if (tlv_type != 0x03) { i += tlv_len; continue; }   // only care about the NDEF Message TLV

        int ndef_end = i + tlv_len;
        while (i < ndef_end && i < data_len) {
            uint8_t hdr      = all_data[i++];
            bool    sr       = (hdr & 0x10) != 0;
            bool    il       = (hdr & 0x08) != 0;
            uint8_t tnf      = hdr & 0x07;
            uint8_t type_len = all_data[i++];

            uint32_t payload_len;
            if (sr) {
                payload_len = all_data[i++];
            } else {
                payload_len = ((uint32_t)all_data[i] << 24) | ((uint32_t)all_data[i + 1] << 16) |
                              ((uint32_t)all_data[i + 2] << 8) | all_data[i + 3];
                i += 4;
            }
            if (il) { uint8_t id_len = all_data[i++]; i += id_len; }

            char rec_type[8] = {0};
            for (int t = 0; t < type_len && t < 7 && i < data_len; t++) rec_type[t] = (char)all_data[i++];

            if (tnf == 0x01 && type_len == 1 && rec_type[0] == 'T' && payload_len > 0) {
                uint8_t status_byte = all_data[i++];
                uint8_t lang_len    = status_byte & 0x3F;
                i += lang_len;
                uint32_t text_len = payload_len - 1 - lang_len;
                size_t   n = (text_len < text_out_sz - 1) ? text_len : text_out_sz - 1;
                for (size_t t = 0; t < n; t++) text_out[t] = (char)all_data[i + t];
                text_out[n] = '\0';
                return true;
            }
            i += (int)payload_len;
        }
        break;
    }
    return false;
}

// nfc_reader_init() sends ANT_ON exactly once, at boot. If that specific
// exchange is lost to a transient wiring glitch during the boot window
// (confirmed possible — a flaky connection at power-on can leave the
// module fully reachable, answering REQUEST cleanly, with its antenna
// never actually turned on, so it senses nothing until a full reboot
// re-runs init), nothing afterward ever retries it. Re-assert
// periodically instead — idempotent (a no-op if the antenna's already
// on) and cheap, so a lost boot-time init can self-heal without a manual
// power-cycle.
#define ANTENNA_REASSERT_INTERVAL_MS 10000

static void reassert_antenna_if_due(void)
{
    static uint32_t s_last_ms = 0;
    uint32_t now = now_ms();
    if (s_last_ms != 0 && now - s_last_ms < ANTENNA_REASSERT_INTERVAL_MS) return;
    s_last_ms = now;
    send_packet(CMD_ANT_ON, NULL, 0);
    recv_packet(NULL, 0, 200);
    // s_healthy tracks whether the link is responding at all (any
    // well-formed reply, logged as NFC_LINK_NORMAL) — not recv_packet's
    // OK-vs-ERR return value. A TAIL_ERR reply to "turn antenna on" (e.g.
    // because it's already on) is a well-formed answer, not a sign of an
    // unhealthy link; using the raw return value here previously flipped
    // nfc_reader_is_healthy() to false on exactly that kind of harmless
    // NAK, reverting the UI from Ready to WifiSpeaker every reassert
    // interval even though the reader was working fine.
    s_healthy = (s_link_state == NFC_LINK_NORMAL);
}

nfc_tap_result_t nfc_reader_poll(char *uri_out, size_t uri_out_sz)
{
    reassert_antenna_if_due();

    uint8_t req_mode = REQ_ALL;
    send_packet(CMD_REQUEST, &req_mode, 1);
    uint8_t tag_type[2];
    if (!recv_packet(tag_type, sizeof(tag_type), 150)) {
        // No card right now — once confirmed by a few consecutive misses
        // (not just one), clears the de-dupe latch below, so the *same*
        // tag can trigger again next time it's genuinely presented
        // (without this, a second tap of the one tag we've been testing
        // with would silently be swallowed as "nothing new" forever after
        // the first successful read).
        if (++s_absence_misses >= UID_ABSENCE_CONFIRM_MISSES) {
            s_last_uid_hex[0] = '\0';
        }
        return NFC_TAP_NONE;
    }
    s_absence_misses = 0;   // card answered a REQUEST — definitely still present

    send_packet(CMD_ANTICOLL, NULL, 0);
    uint8_t uid[4];
    if (!recv_packet(uid, sizeof(uid), 150)) {
        ESP_LOGW(TAG, "tap: card detected (type %02X%02X) but anticoll failed",
                 tag_type[0], tag_type[1]);
        return NFC_TAP_BAD;
    }

    char uid_hex[16];
    snprintf(uid_hex, sizeof(uid_hex), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    if (strcmp(uid_hex, s_last_uid_hex) == 0) return NFC_TAP_NONE;   // same tag still resting on the reader
    strncpy(s_last_uid_hex, uid_hex, sizeof(s_last_uid_hex) - 1);

    ESP_LOGI(TAG, "tap: UID %s (type %02X%02X)", uid_hex, tag_type[0], tag_type[1]);

    // TEMPORARY: the only tag on hand for testing is blank (no NDEF
    // written), so hardcode a real Spotify URI for its specific UID to
    // prove out the Sonos playback path end-to-end. Remove once a
    // properly-written NDEF tag is available.
    if (strcmp(uid_hex, "82BA2006") == 0) {
        static const char *test_uri = "spotify:album:3xmKWmqJFoXS22tePO3mgd";
        ESP_LOGW(TAG, "DIAGNOSTIC: hardcoded test URI for blank test tag: %s", test_uri);
        strncpy(uri_out, test_uri, uri_out_sz - 1);
        uri_out[uri_out_sz - 1] = '\0';
        send_packet(CMD_HALT, NULL, 0);
        recv_packet(NULL, 0, 100);
        return NFC_TAP_GOOD;
    }

    nfc_tap_result_t result = NFC_TAP_BAD;
    if (!select_uid(uid)) {
        ESP_LOGW(TAG, "tap: select failed");
    } else {
        char text[128];
        if (!read_ndef_text(uid, text, sizeof(text))) {
            ESP_LOGW(TAG, "tap: no NDEF text found on card");
        } else {
            ESP_LOGI(TAG, "tap: NDEF text \"%s\"", text);
            if (parse_tag_text(text, uri_out, uri_out_sz)) {
                ESP_LOGI(TAG, "tap: %s", uri_out);
                result = NFC_TAP_GOOD;
            } else {
                ESP_LOGW(TAG, "tap: NDEF text \"%s\" is not a valid service:type:id uri", text);
            }
        }
    }

    // Per the reference library's own usage pattern: halt the card once
    // we're done with it so the reader cleanly settles rather than
    // potentially getting stuck refusing to answer the next REQUEST.
    send_packet(CMD_HALT, NULL, 0);
    recv_packet(NULL, 0, 100);

    return result;
}

bool nfc_reader_is_healthy(void)
{
    return s_healthy;
}
