#include "cloud_asr.h"
#include "cloud_util.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_CLOUD_ASR_USE_XFYUN_WS
#include "xfyun_iat_ws.h"
#endif

#if !CONFIG_CLOUD_ASR_USE_XFYUN_WS
static const char *TAG = "cloud_asr";
#endif

esp_err_t cloud_asr_transcribe_wav(const uint8_t *wav, size_t wav_len, char **out_text)
{
    if (!wav || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

#if CONFIG_CLOUD_ASR_USE_XFYUN_WS
    return xfyun_iat_transcribe_wav(wav, wav_len, out_text);
#else
    if (strlen(CONFIG_CLOUD_ASR_URL) == 0 || strlen(CONFIG_CLOUD_ASR_TOKEN) == 0) {
        ESP_LOGW(TAG, "ASR URL/token not set");
        return ESP_ERR_INVALID_STATE;
    }

    char *resp = NULL;
    esp_err_t err = cloud_http_post_multipart_capture(CONFIG_CLOUD_ASR_URL, CONFIG_CLOUD_ASR_TOKEN,
                                                      (const char *)wav, wav_len, &resp);
    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }
    cJSON *text = cJSON_GetObjectItem(root, "text");
    const char *t = cJSON_IsString(text) ? text->valuestring : NULL;
    if (!t) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    *out_text = strdup(t);
    cJSON_Delete(root);
    return *out_text ? ESP_OK : ESP_ERR_NO_MEM;
#endif
}
