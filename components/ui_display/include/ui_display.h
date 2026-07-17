#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_STATE_IDLE = 0,
    UI_STATE_LISTEN,
    UI_STATE_UPLOAD,
    UI_STATE_THINK,
    UI_STATE_SPEAK,
    UI_STATE_ERROR,
} ui_app_state_t;

esp_err_t ui_display_init(void);

void ui_display_set_state(ui_app_state_t s);

/** UTF-8 文本会按固定上限截断，避免过长导致额外堆分配 */
void ui_display_set_texts(const char *user_line, const char *answer_line);

#ifdef __cplusplus
}
#endif
