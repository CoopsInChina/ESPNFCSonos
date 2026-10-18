#include "buzzer.h"
#include "board_config.h"
#include "app_config.h"

#include "driver/i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>

static const char *TAG = "buzzer";

#define SAMPLE_RATE_HZ 16000
#define I2S_PORT       I2S_NUM_0

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

static void buzzer_task(void *arg)
{
    int count;
    for (;;) {
        if (xQueueReceive(s_queue, &count, portMAX_DELAY) != pdTRUE) continue;
        for (int i = 0; i < count; i++) {
            write_tone(BEEP_DURATION_MS);
            if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(BEEP_GAP_MS));
        }
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

    s_queue = xQueueCreate(4, sizeof(int));
    xTaskCreate(buzzer_task, "buzzer", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "init OK");
}

void buzzer_beep(int count)
{
    if (!s_queue || count <= 0) return;
    xQueueSend(s_queue, &count, 0);
}
