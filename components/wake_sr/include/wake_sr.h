#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 WakeNet，使用 Kconfig WAKENET_MODEL_NAME */
esp_err_t wake_sr_init(void);

void wake_sr_deinit(void);

/** 送入一帧麦克风 PCM（长度须等于 wake_sr_feed_chunk_samples()） */
bool wake_sr_process(int16_t *samples);

int wake_sr_feed_chunk_samples(void);

#ifdef __cplusplus
}
#endif
