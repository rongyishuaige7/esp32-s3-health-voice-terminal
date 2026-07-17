#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_I2S_SAMPLE_RATE   16000
#define AUDIO_I2S_BITS          16

/** 初始化双 I2S：麦克风 + 功放 */
esp_err_t audio_i2s_init(void);

/** 停止 I2S 外设（省电或升级前） */
void audio_i2s_deinit(void);

/**
 * 从麦克风读取 PCM（阻塞），单声道 int16。
 * @param samples_out 输出缓冲区
 * @param samples_count 采样点数（非字节）
 */
esp_err_t audio_i2s_mic_read(int16_t *samples_out, size_t samples_count);

/**
 * 向 MAX98357 写入 PCM 播放。
 */
esp_err_t audio_i2s_spk_write(const int16_t *samples, size_t samples_count);

/** 半双工：暂停/恢复麦克风通道，恢复时会丢弃少量首帧避免残留 DMA 数据 */
void audio_i2s_set_capture_enabled(bool enabled);

/**
 * 一次 TTS 播放结束后调用：停掉 I2S TX DMA，避免 MAX98357
 * 把最后一帧无限循环输出（表现为“最后一个字拖音/不停重复”）。
 * 下次 audio_i2s_spk_write 前会自动重新 enable。
 */
void audio_i2s_spk_stop(void);

#ifdef __cplusplus
}
#endif
