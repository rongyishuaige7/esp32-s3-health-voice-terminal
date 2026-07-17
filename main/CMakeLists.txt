idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES net_wifi audio_i2s wake_sr ui_display cloud_api
        esp_timer esp_psram heap nvs_flash app_update esp_hw_support esp_system
)
