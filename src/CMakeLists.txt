FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

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
