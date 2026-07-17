#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** OpenAI Whisper 兼容 multipart 上传 */
esp_err_t cloud_asr_transcribe_wav(const uint8_t *wav, size_t wav_len, char **out_text);

#ifdef __cplusplus
}
#endif
