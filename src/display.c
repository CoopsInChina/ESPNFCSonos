/**
 * Display bring-up — ST77916 QSPI panel + esp_lvgl_port.
 *
 * Pin map confirmed against docs.waveshare.com/ESP32-S3-Touch-LCD-1.85.
 * The esp_lvgl_port struct shape used below is copied from the sibling
 * Grill & Groove project's own working display.c
 * (/Users/ESP32MusicAndMeat/src/display.c), which pins the identical
 * esp_lvgl_port (^2.3.0) / lvgl (~8.3.0) versions.
 *
 * LCD_RST is NOT a direct GPIO on this board — it's EXIO2 on the onboard
 * TCA9554 IO expander (see tca9554.h). tca9554_init() must be called
 * before display_init() (main.c does this).
 */

#include "display.h"
#include "board_config.h"
#include "app_config.h"
#include "tca9554.h"

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "lcd_init_cmds.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;

#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CH    LEDC_CHANNEL_0
#define BL_LEDC_RES   LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ  20000

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = BL_LEDC_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_LEDC_CH,
        .timer_sel  = BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LCD_BL_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void display_set_backlight(bool on)
{
    display_set_brightness(on ? 100 : 0);
}

void display_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t max_duty = (1u << BL_LEDC_RES) - 1;
    uint32_t duty = (uint32_t)((pct * (int)max_duty) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH);
}

esp_err_t display_init(void)
{
    // 0. Real hardware reset pulse via the IO expander (LCD_RST = EXIO2,
    // not a direct ESP32 GPIO on this board). Timing matches Waveshare's
    // own board config exactly (see lcd_init_cmds.h for the source).
    tca9554_set_pin(TCA_PIN_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    tca9554_set_pin(TCA_PIN_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 1. QSPI bus
    spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK_GPIO, LCD_DATA0_GPIO, LCD_DATA1_GPIO, LCD_DATA2_GPIO, LCD_DATA3_GPIO,
        DISPLAY_WIDTH * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 2. Panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config =
        ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS_GPIO, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_QSPI_HOST,
                                              &io_config, &io_handle));

    // 3. ST77916 panel — with the vendor-specific init sequence (see
    // lcd_init_cmds.h); the driver's generic default sequence is not
    // enough for this exact panel.
    st77916_vendor_config_t vendor_config = {
        .init_cmds      = s_lcd_init_cmds,
        .init_cmds_size = LCD_INIT_CMDS_SIZE,
        .flags          = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,   // reset is driven manually above, via the IO expander
        // Test matrix (via ui_color_test.c): RGB+invert(on) and BGR+invert(off)
        // both tried, neither matched cleanly (some swatches matched pure
        // inversion, others didn't — inconsistent with a single transform).
        // Testing the 3rd combination: RGB (as Waveshare's config literally
        // states) with invert genuinely off.
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io_handle, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // Root cause of the whole color saga: CONFIG_LV_COLOR_16_SWAP was
    // missing, causing a 3-way channel rotation. That's now fixed (see
    // sdkconfig.defaults), confirmed by the color-test pattern showing a
    // *clean* full inversion with no channel mixing — so this just needs
    // to go back to true, matching Waveshare's own confirmed config.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // 4. Backlight (off until the caller turns it on)
    backlight_init();
    display_set_backlight(false);

    // 5. LVGL port
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // Default task_affinity (-1, either core) lets the scheduler land this
    // on core 1 at times, where album_art.c's art_task runs pinned at a
    // much higher priority (10) for its JPEG decode/scale step — that's
    // CPU-bound, not I/O-wait, so when it runs it preempts anything lower
    // priority sharing that core, including this task, freezing the
    // spinner animation for however long the decode takes. Pinning this
    // to core 0 keeps the two from ever contending for the same core.
    lvgl_cfg.task_affinity = 0;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = s_panel,
        .buffer_size   = DISPLAY_WIDTH * DISPLAY_HEIGHT / 10,
        .double_buffer = true,
        .hres          = DISPLAY_WIDTH,
        .vres          = DISPLAY_HEIGHT,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            .sw_rotate   = false,
            // RGB565 byte-swap flag only exists on esp_lvgl_port's LVGL9
            // struct (#if LVGL_VERSION_MAJOR >= 9) — we're pinned to 8.3.x,
            // so the equivalent fix is CONFIG_LV_COLOR_16_SWAP in
            // sdkconfig.defaults instead (LVGL's own internal byte swap).
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL display ready (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

bool display_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void display_unlock(void)              { lvgl_port_unlock(); }
