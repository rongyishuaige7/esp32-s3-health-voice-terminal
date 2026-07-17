idf_component_register(
    SRCS "wake_sr.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp-sr driver freertos esp_system esp_partition
)
