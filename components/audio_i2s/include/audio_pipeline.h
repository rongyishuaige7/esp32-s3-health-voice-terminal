#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 边录边回调：每读一帧麦克风 PCM 调用一次（16k/mono/int16） */
typedef esp_err_t (*audio_pipeline_feed_cb)(void *ctx, const int16_t *pcm, size_t sample_count);

/**
 * 与 record_until_silence_or_max 相同的静音结束逻辑，但不缓存整段 PCM；
 * 每帧调用 feed_cb。feed_cb 返回非 ESP_OK 时提前结束并透传错误码。
 */
esp_err_t audio_pipeline_record_stream(audio_pipeline_feed_cb feed_cb, void *ctx, int max_ms);

/** 录音缓冲：唤醒后持续写入，直到 stop 或超时 */
esp_err_t audio_pipeline_record_start(void);

/**
 * 从麦克风拉取并累积到内部缓冲，最长 max_ms 毫秒或 RMS 低于阈值时提前结束。
 * @param out_pcm 输出堆指针（malloc），调用方负责 free
 * @param out_bytes PCM 字节数
 */
esp_err_t audio_pipeline_record_until_silence_or_max(int max_ms, uint8_t **out_pcm, size_t *out_bytes);

void audio_pipeline_record_abort(void);

/** 生成带 WAV 头的缓冲区（便于上传 Whisper） */
esp_err_t audio_pcm_add_wav_header(const uint8_t *pcm, size_t pcm_len,
                                   uint8_t **out_wav, size_t *out_wav_len);

#ifdef __cplusplus
}
#endif
