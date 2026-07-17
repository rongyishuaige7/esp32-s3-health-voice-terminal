#include "ui_display.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_task_wdt.h"
#include "lvgl.h"

LV_FONT_DECLARE(lv_font_sc_supplement_16);

static const char *TAG = "ui";
#define UI_LABEL_TEXT_MAX_BYTES 192
/** 问句与答句之间的竖向间距，避免多行问句时首行与答句重叠 */
#define UI_QA_GAP_PX 16

/* 思源 16 CJK 子集较小；fallback 覆盖中文标点/全角符号与 U+4E00–U+9FFF 常用 CJK 基本区。 */
static lv_font_t s_font_han_chain;
static bool s_font_chain_ready;

static void ui_font_chain_init(void)
{
    if (s_font_chain_ready) {
        return;
    }
    s_font_han_chain = lv_font_source_han_sans_sc_16_cjk;
    s_font_han_chain.fallback = &lv_font_sc_supplement_16;
    s_font_chain_ready = true;
}

static lv_obj_t *s_lbl_state;
static lv_obj_t *s_lbl_user;
static lv_obj_t *s_lbl_answer;

static bool utf8_is_continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

static size_t utf8_decode_char(const char *src, uint32_t *codepoint, bool *valid)
{
    const unsigned char c0 = (unsigned char)src[0];
    *valid = true;

    if ((c0 & 0x80) == 0) {
        *codepoint = c0;
        return 1;
    }
    if ((c0 & 0xE0) == 0xC0 && src[1] != '\0') {
        const unsigned char c1 = (unsigned char)src[1];
        const uint32_t cp = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
        if (utf8_is_continuation(c1) && cp >= 0x80) {
            *codepoint = cp;
            return 2;
        }
    }
    if ((c0 & 0xF0) == 0xE0 && src[1] != '\0' && src[2] != '\0') {
        const unsigned char c1 = (unsigned char)src[1];
        const unsigned char c2 = (unsigned char)src[2];
        const uint32_t cp = ((uint32_t)(c0 & 0x0F) << 12) |
                            ((uint32_t)(c1 & 0x3F) << 6) |
                            (uint32_t)(c2 & 0x3F);
        if (utf8_is_continuation(c1) && utf8_is_continuation(c2) &&
            cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
            *codepoint = cp;
            return 3;
        }
    }
    if ((c0 & 0xF8) == 0xF0 && src[1] != '\0' && src[2] != '\0' && src[3] != '\0') {
        const unsigned char c1 = (unsigned char)src[1];
        const unsigned char c2 = (unsigned char)src[2];
        const unsigned char c3 = (unsigned char)src[3];
        const uint32_t cp = ((uint32_t)(c0 & 0x07) << 18) |
                            ((uint32_t)(c1 & 0x3F) << 12) |
                            ((uint32_t)(c2 & 0x3F) << 6) |
                            (uint32_t)(c3 & 0x3F);
        if (utf8_is_continuation(c1) && utf8_is_continuation(c2) &&
            utf8_is_continuation(c3) && cp >= 0x10000 && cp <= 0x10FFFF) {
            *codepoint = cp;
            return 4;
        }
    }

    *valid = false;
    *codepoint = '?';
    return 1;
}

static bool ui_font_chain_has_glyph(uint32_t codepoint)
{
    if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t') {
        return true;
    }
    if (!s_font_chain_ready) {
        return true;
    }

    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(&s_font_han_chain, &dsc, codepoint, 0) &&
           !dsc.is_placeholder && dsc.resolved_font != NULL;
}

static size_t copy_glyph_or_placeholder(char *dst, size_t dst_sz, const char *src)
{
    uint32_t codepoint;
    bool valid;
    const size_t seq = utf8_decode_char(src, &codepoint, &valid);
    const bool use_placeholder = !valid || !ui_font_chain_has_glyph(codepoint);
    const size_t out_len = use_placeholder ? 1 : seq;

    if (out_len > dst_sz) {
        return 0;
    }

    if (use_placeholder) {
        dst[0] = '?';
        return 1;
    }

    memcpy(dst, src, seq);
    return seq;
}

static void copy_label_text(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t in = 0;
    size_t out = 0;
    while (src[in] != '\0') {
        size_t written = copy_glyph_or_placeholder(dst + out, dst_sz - 1 - out, src + in);
        if (written == 0) {
            break;
        }
        uint32_t codepoint;
        bool valid;
        const size_t consumed = utf8_decode_char(src + in, &codepoint, &valid);
        out += written;
        in += consumed;
    }
    dst[out] = '\0';
}

static const char *state_str(ui_app_state_t s)
{
    switch (s) {
    case UI_STATE_IDLE:
        return "Idle";
    case UI_STATE_LISTEN:
        return "Listening";
    case UI_STATE_UPLOAD:
        return "Upload";
    case UI_STATE_THINK:
        return "Thinking";
    case UI_STATE_SPEAK:
        return "Speaking";
    case UI_STATE_ERROR:
        return "Error";
    default:
        return "?";
    }
}

static void ui_apply_state_label(ui_app_state_t s)
{
    if (!s_lbl_state) {
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "State: %s", state_str(s));
    lv_label_set_text(s_lbl_state, buf);
}

void ui_display_set_state(ui_app_state_t s)
{
    if (lvgl_port_lock(2000)) {
        ui_apply_state_label(s);
        lvgl_port_unlock();
    }
}

void ui_display_set_texts(const char *user_line, const char *answer_line)
{
    char user_buf[UI_LABEL_TEXT_MAX_BYTES];
    char answer_buf[UI_LABEL_TEXT_MAX_BYTES];

    if (!lvgl_port_lock(2000)) {
        return;
    }
    if (s_lbl_user && user_line) {
        copy_label_text(user_buf, sizeof(user_buf), user_line);
        lv_label_set_text(s_lbl_user, user_buf);
    }
    if (s_lbl_answer && answer_line) {
        copy_label_text(answer_buf, sizeof(answer_buf), answer_line);
        lv_label_set_text(s_lbl_answer, answer_buf);
    }
    if (s_lbl_user && s_lbl_answer) {
        lv_obj_update_layout(s_lbl_user);
        lv_obj_align_to(s_lbl_answer, s_lbl_user, LV_ALIGN_OUT_BOTTOM_LEFT, 0, UI_QA_GAP_PX);
    }
    lvgl_port_unlock();
}

static void build_widgets(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    ui_font_chain_init();

    s_lbl_state = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_state, &s_font_han_chain, 0);
    lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(0x88c0d0), 0);
    lv_label_set_text(s_lbl_state, "State: Init");
    lv_obj_align(s_lbl_state, LV_ALIGN_TOP_MID, 0, 6);

    s_lbl_user = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_user, &s_font_han_chain, 0);
    lv_obj_set_width(s_lbl_user, 220);
    lv_label_set_long_mode(s_lbl_user, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_lbl_user, lv_color_hex(0xeceff4), 0);
    lv_label_set_text(s_lbl_user, "Q: -");
    lv_obj_align(s_lbl_user, LV_ALIGN_TOP_LEFT, 8, 32);

    s_lbl_answer = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_answer, &s_font_han_chain, 0);
    lv_obj_set_width(s_lbl_answer, 220);
    lv_label_set_long_mode(s_lbl_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_lbl_answer, lv_color_hex(0xa3be8c), 0);
    lv_label_set_text(s_lbl_answer, "A: -");
    /* 随问题区高度变化下移，减少长问句与答案重叠 */
    lv_obj_align_to(s_lbl_answer, s_lbl_user, LV_ALIGN_OUT_BOTTOM_LEFT, 0, UI_QA_GAP_PX);
}

esp_err_t ui_display_init(void)
{
    gpio_config_t bk = {
        .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bk);
    gpio_set_level(BOARD_LCD_PIN_BL, 1);
    esp_task_wdt_reset();

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 240 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi");
    esp_task_wdt_reset();

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        /* 16MHz：WiFi/TLS + TTS WS 并发时再低一档，避免 GDMA 与网络 DMA 抢带宽。
         * 现象：20MHz 下 LLM 流式刷新叠加 TTS 收包会触发 panel_io_spi_tx_color queue failed，
         * 一旦失败 flush 回调不再 notify LVGL，整个 UI 卡死，连带 TTS 播放队列阻塞。 */
        .pclk_hz = 16 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        /* 队列深度调大：一次 LVGL flush 可能分成多段 DMA（bounce buffer 逐段上屏），
         * 深度不足会返回 ESP_ERR_NO_MEM 导致 tx_color failed。 */
        .trans_queue_depth = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io), TAG, "io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pcfg, &panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "rst");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "on");
    esp_task_wdt_reset();

    lvgl_port_cfg_t lv_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* 把 LVGL 任务钉到 Core0，让 Core1 专给 TTS WS / I2S 播放 / 唤醒词，
     * 避免 LCD SPI flush 阻塞 spk_pipe_task 造成播放一段一段。 */
    lv_cfg.task_affinity = 0;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lv_cfg), TAG, "lvgl");
    esp_task_wdt_reset();

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .control_handle = NULL,
        /* 10 行单缓冲 = ~4.8KB 内部 DMA RAM，配合 PSRAM mbedTLS 动态分配
         * 可同时满足 LCD DMA + TLS 握手峰值，不再 OOM。
         * 单缓冲够用：LVGL 会阻塞等 flush 完成，副作用是刷新略慢，但流式文本感官无差。 */
        .buffer_size = 240 * 10,
        .double_buffer = false,
        .hres = 240,
        .vres = 240,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            /* 关键改动：buffer 放内部 DMA 可达内存，不再让 esp_lcd bounce。
             * 否则 PSRAM→内部 SRAM 的 bounce 会把一次 flush 切成多段 SPI 事务，
             * 在 WiFi/TLS 抢带宽时极易填满 trans_queue 造成 tx_color failed。 */
            .buff_dma = 1,
            .buff_spiram = 0,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = 1,
#endif
        },
    };

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        return ESP_FAIL;
    }
    esp_task_wdt_reset();

    if (lvgl_port_lock(2000)) {
        build_widgets();
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "UI OK");
    return ESP_OK;
}
