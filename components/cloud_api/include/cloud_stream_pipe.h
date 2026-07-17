#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化流式播放管线（TTS 合成任务 + PCM StreamBuffer + 扬声器任务）。可重复调用，仅首次创建任务。
 */
esp_err_t cloud_stream_pipe_init(void);

/**
 * 一轮对话在 ASR 完成之后、调用 DeepSeek 流式与 TTS 之前调用：
 * 关闭麦克风、清空 PCM 缓冲、复位事件位。
 */
void cloud_stream_pipe_prepare_playback(void (*on_first_pcm)(void *ctx), void *ctx);

/** 将 LLM 切出的句子送入 TTS 队列（取得堆指针所有权） */
esp_err_t cloud_stream_pipe_enqueue_sentence(char *sentence_utf8);

/** LLM 流结束后调用：通知 TTS 队列不再有新句 */
void cloud_stream_pipe_mark_llm_sentences_done(void);

/** 阻塞直到本轮 PCM 播放完毕（含尾音） */
esp_err_t cloud_stream_pipe_wait_playback_done(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
