#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *user_text;
    char *answer_text;
    uint8_t *tts_pcm;
    size_t tts_pcm_bytes;
} cloud_chain_result_t;

void cloud_chain_result_free(cloud_chain_result_t *r);

/** ASR+归一化完成后回调（在 DeepSeek 之前），便于立即刷新 UI */
typedef void (*cloud_chain_on_asr_cb)(const char *normalized_user_utf8, void *user_ctx);

typedef struct {
    cloud_chain_on_asr_cb on_asr;
    void *on_asr_ctx;
} cloud_chain_opts_t;

/**
 * WAV -> ASR -> 医疗归一 -> DeepSeek ->（可选）TTS
 * ASR/归一/LLM 等关键步骤失败返回错误码；TTS 失败仅记录警告，仍可能返回 ESP_OK，部分结果仍可能写入 result。
 */
esp_err_t cloud_chain_run(const uint8_t *wav, size_t wav_len, cloud_chain_result_t *out);

/** 同 cloud_chain_run，可选 ASR 完成后回调（opts 可为 NULL） */
esp_err_t cloud_chain_run_ex(const uint8_t *wav, size_t wav_len, cloud_chain_result_t *out,
                             const cloud_chain_opts_t *opts);

#if CONFIG_CHAIN_STREAMING
/** 讯飞 IAT 流式过程中识别文本增量（可为 NULL） */
typedef void (*cloud_chain_on_asr_partial_cb)(const char *asr_text_so_far, void *ctx);

typedef struct {
    cloud_chain_on_asr_cb on_asr;
    void *on_asr_ctx;
    cloud_chain_on_asr_partial_cb on_asr_partial;
    void *on_asr_partial_ctx;
    void (*on_answer_delta)(const char *answer_so_far, void *ctx);
    void *on_answer_delta_ctx;
    /** 扬声器即将播放首包 PCM 时（可选） */
    void (*on_first_spoken_pcm)(void *ctx);
    void *on_first_spoken_pcm_ctx;
} cloud_chain_stream_opts_t;

/**
 * 边录边传 ASR -> 流式 DeepSeek -> 按句讯飞 TTS -> 扬声器（须已 cloud_stream_pipe_init）
 */
esp_err_t cloud_chain_run_streaming(const cloud_chain_stream_opts_t *opts, cloud_chain_result_t *out);
#endif

#ifdef __cplusplus
}
#endif
