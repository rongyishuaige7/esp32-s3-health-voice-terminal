#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cloud_deepseek_chat(const char *system_prompt, const char *user_message,
                              char **out_reply);

/** 收到可送 TTS 的短句（堆指针，回调取得所有权并应 free） */
typedef void (*cloud_deepseek_sentence_cb)(char *sentence_utf8, void *ctx);

/**
 * SSE 流式 chat：按句回调 on_sentence；on_full_delta 可选，用于 UI 刷全文。
 * 句末按标点切分，末段 force flush。
 */
esp_err_t cloud_deepseek_chat_stream(const char *system_prompt, const char *user_message,
                                     cloud_deepseek_sentence_cb on_sentence,
                                     void (*on_full_delta)(const char *answer_so_far, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
