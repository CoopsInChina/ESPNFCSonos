FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.h)

# Static UI images (ui_assets.c) + the NFC-tap sound (buzzer.c) are plain
# generated C byte-array sources under src/assets/ (boot_jpg.c, etc.),
# picked up by the glob above like any other source — not idf_component_
# register's EMBED_FILES, which generates mangled/doubled intermediate
# .S-file paths under PlatformIO's ESP-IDF integration (it computes those
# paths assuming idf.py's native directory layout). Regenerate them from
# the original .jpg/.wav files with the script noted in README.md's
# "Asset pipeline" section if the source assets ever change.

idf_component_register(
    SRCS ${app_sources}
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_timer
        lwip
        heap
        freertos
        driver
        esp_lcd
        esp_lcd_st77916
        esp_lvgl_port
        esp_partition
        mbedtls
)
