idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES bsp_extra esp_wifi esp_event esp_netif nvs_flash esp_timer espressif__esp_audio_simple_player esp_http_client json
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
)
