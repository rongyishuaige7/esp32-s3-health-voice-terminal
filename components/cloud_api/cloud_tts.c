#include "cloud_tts.h"
#include "cloud_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_CLOUD_TTS_USE_XFYUN_WS
#include "xfyun_tts_ws.h"
#endif

#if !CONFIG_CLOUD_TTS_USE_XFYUN_WS
static const char *TAG = "cloud_tts";
static bool tts_content_type_supported(const char *content_type)
{
    if (!content_type || content_type[0] == '\0') {
        return true;
    }
    if (strncasecmp(content_type, "audio/", 6) == 0) {
        return true;
    }
    if (strncasecmp(content_type, "application/octet-stream", 24) == 0) {
        return true;
    }
    return false;
}

static bool tts_strip_wav_header(uint8_t *pcm, size_t *pcm_len)
{
    if (!pcm || !pcm_len || *pcm_len < 44) {
        return false;
    }
    if (memcmp(pcm, "RIFF", 4) != 0 || memcmp(pcm + 8, "WAVE", 4) != 0) {
        return false;
    }

    memmove(pcm, pcm + 44, *pcm_len - 44);
    *pcm_len -= 44;
    return true;
}
#endif

esp_err_t cloud_tts_synthesize(const char *text, uint8_t **out_pcm, size_t *out_len)
{
    if (!text || !out_pcm || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_pcm = NULL;
    *out_len = 0;

#if CONFIG_CLOUD_TTS_USE_XFYUN_WS
    return xfyun_tts_synthesize_pcm(text, out_pcm, out_len);
#else
    if (strlen(CONFIG_CLOUD_TTS_URL) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "text", text);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    const char *tok = strlen(CONFIG_CLOUD_TTS_TOKEN) > 0 ? CONFIG_CLOUD_TTS_TOKEN : CONFIG_CLOUD_ASR_TOKEN;

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    char *content_type = NULL;
    esp_err_t err = cloud_http_post_binary_response(CONFIG_CLOUD_TTS_URL, tok, json,
                                                    &bin, &bin_len, &content_type);
    free(json);
    if (err != ESP_OK) {
        free(content_type);
        free(bin);
        return err;
    }
    if (!tts_content_type_supported(content_type)) {
        ESP_LOGE(TAG, "unsupported TTS content-type: %s", content_type ? content_type : "(null)");
        free(content_type);
        free(bin);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (tts_strip_wav_header(bin, &bin_len)) {
        ESP_LOGW(TAG, "TTS response was WAV, stripped 44-byte header");
    }
    free(content_type);
    if (bin_len < sizeof(int16_t) || (bin_len % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "invalid TTS PCM length: %u", (unsigned)bin_len);
        free(bin);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_pcm = bin;
    *out_len = bin_len;
    ESP_LOGI(TAG, "tts pcm bytes=%u", (unsigned)bin_len);
    return ESP_OK;
#endif
}
