idf_component_register(
    SRCS "ui_display.c" "lv_font_sc_supplement_16.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_lcd esp_lvgl_port lvgl driver esp_timer esp_system
)
