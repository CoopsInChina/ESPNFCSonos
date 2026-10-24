#include "buzzer.h"
#include "board_config.h"
#include "app_config.h"

#include "driver/i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <string.h>

static const char *TAG = "buzzer";

#define SAMPLE_RATE_HZ 16000
#define I2S_PORT       I2S_NUM_0

// Generated C byte array (see src/assets/nfc_tap_wav.h), converted from
// the provided NfcTap.mp3 — this project has no MP3 decoder, and doesn't
// need one: it's just raw PCM after a small RIFF header, matching this
// I2S config exactly (16-bit, stereo, 16kHz) so it streams straight out
// with no resampling. Not idf_component_register's EMBED_FILES, which
// mangles its intermediate .S-file paths under PlatformIO's ESP-IDF
// integration — see README.md's "Asset pipeline" section to regenerate
// this if the source .wav ever changes.
#include "assets/nfc_tap_wav.h"

typedef enum { CMD_BEEP, CMD_TAP_SOUND } buzzer_cmd_kind_t;
typedef struct { buzzer_cmd_kind_t kind; int count; } buzzer_cmd_t;

static QueueHandle_t s_queue;

static void write_tone(uint32_t duration_ms)
{
    int n_samples = (SAMPLE_RATE_HZ * duration_ms) / 1000;
    int16_t stereo[2];   // one L/R frame per i2s_write call — simple, not fast, fine for a beep
    int period = SAMPLE_RATE_HZ / BEEP_FREQ_HZ;

    for (int i = 0; i < n_samples; i++) {
        // Square wave — loud and clear through a small speaker, cheap to generate.
        int16_t s = ((i % period) < period / 2) ? 9000 : -9000;
        stereo[0] = s;
        stereo[1] = s;
        size_t written = 0;
        i2s_write(I2S_PORT, stereo, sizeof(stereo), &written, portMAX_DELAY);
    }
}

typedef struct __attribute__((packed)) {
    char     riff_id[4];
    uint32_t riff_size;
    char     wave_id[4];
} wav_riff_hdr_t;

typedef struct __attribute__((packed)) {
    char     id[4];
    uint32_t size;
} wav_chunk_hdr_t;

// Walks the embedded WAV's RIFF chunks to find "data" — can't assume a fixed
// header size, afconvert pads this one's header out to a 4096-byte offset.
static void play_tap_wav(void)
{
    const uint8_t *p   = nfc_tap_wav_data;
    const uint8_t *end = nfc_tap_wav_data + nfc_tap_wav_len;

    if ((size_t)(end - p) < sizeof(wav_riff_hdr_t)) return;
    const wav_riff_hdr_t *riff = (const wav_riff_hdr_t *)p;
    if (memcmp(riff->riff_id, "RIFF", 4) != 0 || memcmp(riff->wave_id, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "tap sound: not a RIFF/WAVE file");
        return;
    }
    p += sizeof(wav_riff_hdr_t);

    const uint8_t *data = NULL;
    uint32_t       data_len = 0;
    while ((size_t)(end - p) >= sizeof(wav_chunk_hdr_t)) {
        const wav_chunk_hdr_t *chunk      = (const wav_chunk_hdr_t *)p;
        const uint8_t         *chunk_data = p + sizeof(wav_chunk_hdr_t);
        if (memcmp(chunk->id, "data", 4) == 0) {
            data = chunk_data;
            data_len = chunk->size;
            break;
        }
        p = chunk_data + chunk->size + (chunk->size & 1);   // chunks are word-aligned
    }
    if (!data || data_len == 0) {
        ESP_LOGW(TAG, "tap sound: no data chunk found");
        return;
    }
    if (data + data_len > end) data_len = (uint32_t)(end - data);   // clamp, just in case

    size_t written = 0;
    i2s_write(I2S_PORT, data, data_len, &written, portMAX_DELAY);
}

static void buzzer_task(void *arg)
{
    buzzer_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        if (cmd.kind == CMD_TAP_SOUND) {
            play_tap_wav();
        } else {
            for (int i = 0; i < cmd.count; i++) {
                write_tone(BEEP_DURATION_MS);
                if (i + 1 < cmd.count) vTaskDelay(pdMS_TO_TICKS(BEEP_GAP_MS));
            }
        }

        // i2s_write() only loads the DMA ring buffer — once the sound's last
        // frames are written, the DMA keeps looping that same last buffer
        // (there's no "end of stream" in I2S) rather than going quiet.
        // Overwrite it with silence once playback is done.
        i2s_zero_dma_buffer(I2S_PORT);
    }
}

void buzzer_init(void)
{
    i2s_config_t cfg = {
        .mode                 = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate          = SAMPLE_RATE_HZ,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 4,
        .dma_buf_len          = 256,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK_GPIO,
        .ws_io_num    = I2S_WS_GPIO,
        .data_out_num = I2S_DOUT_GPIO,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };

    if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK ||
        i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed — check board_config.h I2S pins (currently unverified)");
        return;
    }

    s_queue = xQueueCreate(4, sizeof(buzzer_cmd_t));
    xTaskCreate(buzzer_task, "buzzer", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "init OK");
}

void buzzer_beep(int count)
{
    if (!s_queue || count <= 0) return;
    buzzer_cmd_t cmd = { .kind = CMD_BEEP, .count = count };
    xQueueSend(s_queue, &cmd, 0);
}

void buzzer_play_tap_sound(void)
{
    if (!s_queue) return;
    buzzer_cmd_t cmd = { .kind = CMD_TAP_SOUND, .count = 0 };
    xQueueSend(s_queue, &cmd, 0);
}
