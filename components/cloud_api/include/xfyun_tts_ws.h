#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 讯飞在线语音合成流式 WebAPI（文档：在线语音合成 API）
 * wss://tts-api.xfyun.cn/v2/tts ，返回 PCM 片段 base64，拼成 16bit/mono（aue=raw, 16k）。
 */
esp_err_t xfyun_tts_synthesize_pcm(const char *utf8_text, uint8_t **out_pcm, size_t *out_len);

/** 边收边解码 PCM（16bit/mono/16k LE），按服务端返回的 audio 帧回调 */
esp_err_t xfyun_tts_synthesize_pcm_stream(const char *utf8_text,
                                         void (*on_pcm_chunk)(void *ctx, const uint8_t *pcm, size_t pcm_bytes),
                                         void *user_ctx);

#ifdef __cplusplus
}
#endif
