#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 期望服务端返回原始 PCM 16k/mono/16bit；若未配置 URL 则返回 ESP_ERR_NOT_SUPPORTED */
esp_err_t cloud_tts_synthesize(const char *text, uint8_t **out_pcm, size_t *out_len);

#ifdef __cplusplus
}
#endif
