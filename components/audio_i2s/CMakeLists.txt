idf_component_register(
    SRCS "audio_i2s.c" "audio_pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_common esp_system freertos esp_timer
)
