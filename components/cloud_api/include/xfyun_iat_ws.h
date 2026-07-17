#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 不透明句柄：边录边传的讯飞 IAT 会话 */
typedef struct xfyun_iat_stream xfyun_iat_stream_t;

/**
 * 讯飞「语音听写（流式版）」WebSocket：wss://iat-api.xfyun.cn/v2/iat
 * @param wav 原始 WAV（标准 44 字节头 + 16k/mono/16bit PCM）或裸 PCM（若不以 RIFF 开头则按裸 PCM 发送）
 */
esp_err_t xfyun_iat_transcribe_wav(const uint8_t *wav, size_t wav_len, char **out_text);

/**
 * 开始流式听写会话（建立 WebSocket，内部启动发送任务）。
 * @param on_partial 可选；每次服务端返回增量识别结果时回调当前完整文本
 */
esp_err_t xfyun_iat_stream_begin(xfyun_iat_stream_t **out_ctx, void (*on_partial)(const char *full_text_utf8, void *user),
                                 void *user);

/** 喂入 PCM 字节（16k/mono/16bit），可多次调用 */
esp_err_t xfyun_iat_stream_feed(xfyun_iat_stream_t *ctx, const uint8_t *pcm, size_t len);

/** 标记录音已结束，发送端将在发完缓冲后发送 status=2 */
void xfyun_iat_stream_mark_pcm_done(xfyun_iat_stream_t *ctx);

/**
 * 等待识别完成，销毁连接；成功时 *out_text 为 strdup 堆指针，调用方 free。
 * 若听写终包无有效字词：返回 ESP_ERR_NOT_FOUND（与云端协议/连接错误区分）。
 */
esp_err_t xfyun_iat_stream_finish(xfyun_iat_stream_t *ctx, char **out_text);

/** 出错时中止会话（仍须调用 xfyun_iat_stream_finish 回收资源，或内部 transcribe 已处理） */
void xfyun_iat_stream_abort(xfyun_iat_stream_t *ctx);

#ifdef __cplusplus
}
#endif
