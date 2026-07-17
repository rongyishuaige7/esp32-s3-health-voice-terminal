/**
 * 讯飞开放平台「在线语音合成」WebSocket 流式 API
 * 对齐：https://www.xfyun.cn/doc/tts/online_tts/API.html
 */
#include "xfyun_tts_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cloud_util.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

static const char *TAG = "xfyun_tts";

#define XFYUN_TTS_HOST_STR "tts-api.xfyun.cn"
#define XFYUN_TTS_PATH     "/v2/tts"

typedef struct {
    esp_websocket_client_handle_t client;
    SemaphoreHandle_t done_sem;
    char *json_buf;
    size_t json_len;
    size_t json_cap;
    uint8_t *pcm;
    size_t pcm_len;
    size_t pcm_cap;
    esp_err_t err;
    bool sent_text;
    bool finished;
    char *request_json;
    void (*on_pcm_chunk)(void *ctx, const uint8_t *pcm, size_t pcm_bytes);
    void *on_pcm_ctx;
} xfyun_ctx_t;

static esp_err_t append_pcm_b64(xfyun_ctx_t *ctx, const char *b64)
{
    if (!b64 || !*b64) {
        return ESP_OK;
    }
    if (ctx->on_pcm_chunk) {
        size_t sl = strlen(b64);
        size_t bin_max = sl * 3 / 4 + 16;
        uint8_t *tmp = malloc(bin_max);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        size_t outl = 0;
        if (mbedtls_base64_decode(tmp, bin_max, &outl, (const unsigned char *)b64, sl) != 0) {
            free(tmp);
            return ESP_FAIL;
        }
        if (outl > 0) {
            ctx->on_pcm_chunk(ctx->on_pcm_ctx, tmp, outl);
        }
        free(tmp);
        return ESP_OK;
    }
    size_t sl = strlen(b64);
    size_t bin_len = sl * 3 / 4 + 8;
    size_t need = ctx->pcm_len + bin_len;
    if (need > ctx->pcm_cap) {
        size_t ncap = ctx->pcm_cap ? ctx->pcm_cap * 2 : 8192;
        while (ncap < need) {
            ncap *= 2;
        }
        uint8_t *nb = realloc(ctx->pcm, ncap);
        if (!nb) {
            return ESP_ERR_NO_MEM;
        }
        ctx->pcm = nb;
        ctx->pcm_cap = ncap;
    }
    size_t outl = 0;
    if (mbedtls_base64_decode(ctx->pcm + ctx->pcm_len, ctx->pcm_cap - ctx->pcm_len, &outl,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        return ESP_FAIL;
    }
    ctx->pcm_len += outl;
    return ESP_OK;
}

static void parse_one_json(xfyun_ctx_t *ctx, const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGD(TAG, "json parse fail, len=%u", (unsigned)strlen(json_str));
        return;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "xfyun err code=%d msg=%s", (int)code->valueint,
                 cJSON_IsString(msg) ? msg->valuestring : "?");
        ctx->err = ESP_FAIL;
        ctx->finished = true;
        cJSON_Delete(root);
        xSemaphoreGive(ctx->done_sem);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data && cJSON_IsObject(data)) {
        cJSON *aud = cJSON_GetObjectItem(data, "audio");
        if (cJSON_IsString(aud)) {
            if (append_pcm_b64(ctx, aud->valuestring) != ESP_OK) {
                ctx->err = ESP_FAIL;
                ctx->finished = true;
                cJSON_Delete(root);
                xSemaphoreGive(ctx->done_sem);
                return;
            }
        }
        cJSON *st = cJSON_GetObjectItem(data, "status");
        if (cJSON_IsNumber(st) && (int)st->valuedouble == 2) {
            ctx->err = ESP_OK;
            ctx->finished = true;
            cJSON_Delete(root);
            xSemaphoreGive(ctx->done_sem);
            return;
        }
    }

    cJSON_Delete(root);
}

static void json_buf_append(xfyun_ctx_t *ctx, const char *chunk, int chunk_len)
{
    if (chunk_len <= 0) {
        return;
    }
    size_t need = ctx->json_len + (size_t)chunk_len + 1;
    if (need > ctx->json_cap) {
        size_t ncap = ctx->json_cap ? ctx->json_cap * 2 : 4096;
        while (ncap < need) {
            ncap *= 2;
        }
        char *nb = realloc(ctx->json_buf, ncap);
        if (!nb) {
            ctx->err = ESP_ERR_NO_MEM;
            return;
        }
        ctx->json_buf = nb;
        ctx->json_cap = ncap;
    }
    memcpy(ctx->json_buf + ctx->json_len, chunk, (size_t)chunk_len);
    ctx->json_len += (size_t)chunk_len;
    ctx->json_buf[ctx->json_len] = '\0';
}

static void on_data_fin(xfyun_ctx_t *ctx)
{
    if (!ctx->json_buf || ctx->json_len == 0) {
        return;
    }
    parse_one_json(ctx, ctx->json_buf);
    ctx->json_len = 0;
}

static void ws_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    xfyun_ctx_t *ctx = (xfyun_ctx_t *)handler_args;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        if (!ctx->sent_text && ctx->request_json) {
            int n = strlen(ctx->request_json);
            if (esp_websocket_client_send_text(ctx->client, ctx->request_json, n, pdMS_TO_TICKS(10000)) < 0) {
                ESP_LOGE(TAG, "send_text failed");
                ctx->err = ESP_FAIL;
                ctx->finished = true;
                xSemaphoreGive(ctx->done_sem);
            } else {
                ctx->sent_text = true;
                ESP_LOGI(TAG, "tts request sent, len=%d", n);
            }
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        /* 大包多分帧：续帧 opcode 为 CONT(0)，仅接受 TEXT(1) 会丢片 */
        if (ev->data_ptr && ev->data_len > 0 &&
            (ev->op_code == WS_TRANSPORT_OPCODES_TEXT || ev->op_code == WS_TRANSPORT_OPCODES_CONT)) {
            json_buf_append(ctx, ev->data_ptr, ev->data_len);
            /* 单帧负载 > buffer 时同一帧会多次 DATA，每段都带该帧的 fin=true；仅当本帧 payload 读满且 fin 时再解析 */
            bool frame_done = (ev->payload_len > 0) &&
                              (ev->payload_offset + ev->data_len >= ev->payload_len);
            if (frame_done && ev->fin) {
                on_data_fin(ctx);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR: {
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR type=%d tls=%d ws_hs=%d",
                 (int)ev->error_handle.error_type,
                 (int)ev->error_handle.esp_tls_last_esp_err,
                 ev->error_handle.esp_ws_handshake_status_code);
        if (!ctx->finished) {
            ctx->err = ESP_FAIL;
            ctx->finished = true;
            xSemaphoreGive(ctx->done_sem);
        }
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (!ctx->finished) {
            ESP_LOGW(TAG, "disconnected before done");
            ctx->err = ESP_ERR_INVALID_STATE;
            ctx->finished = true;
            xSemaphoreGive(ctx->done_sem);
        }
        break;

    default:
        break;
    }
}

static char *build_request_json(const char *utf8_text)
{
    size_t tlen = strlen(utf8_text);
    if (tlen > 7990) {
        ESP_LOGW(TAG, "truncate text from %u to 7990 bytes", (unsigned)tlen);
        tlen = 7990;
    }

    const unsigned char *ptext = (const unsigned char *)utf8_text;
    size_t b64_len = 4 * ((tlen + 2) / 3) + 8;
    char *b64 = (char *)malloc(b64_len + 1);
    if (!b64) {
        return NULL;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, b64_len + 1, &olen, ptext, tlen) != 0) {
        free(b64);
        return NULL;
    }
    b64[olen] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON *common = cJSON_CreateObject();
    cJSON *business = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !common || !business || !data) {
        free(b64);
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(common, "app_id", CONFIG_XFYUN_APP_ID);
    cJSON_AddItemToObject(root, "common", common);

    cJSON_AddStringToObject(business, "aue", "raw");
    cJSON_AddStringToObject(business, "auf", "audio/L16;rate=16000");
    cJSON_AddStringToObject(business, "vcn", CONFIG_XFYUN_TTS_VCN);
    cJSON_AddStringToObject(business, "tte", "UTF8");
    cJSON_AddNumberToObject(business, "speed", 50);
    cJSON_AddNumberToObject(business, "volume", 50);
    cJSON_AddItemToObject(root, "business", business);

    cJSON_AddNumberToObject(data, "status", 2);
    cJSON_AddStringToObject(data, "text", b64);
    cJSON_AddItemToObject(root, "data", data);

    free(b64);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

esp_err_t xfyun_tts_synthesize_pcm(const char *utf8_text, uint8_t **out_pcm, size_t *out_len)
{
    if (!utf8_text || !out_pcm || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_pcm = NULL;
    *out_len = 0;

    if (strlen(CONFIG_XFYUN_APP_ID) == 0 || strlen(CONFIG_XFYUN_API_KEY) == 0 ||
        strlen(CONFIG_XFYUN_API_SECRET) == 0) {
        ESP_LOGE(TAG, "XFYUN AppID/APIKey/APISecret not set");
        return ESP_ERR_INVALID_STATE;
    }

    char *uri = (char *)malloc(2048);
    if (!uri) {
        return ESP_ERR_NO_MEM;
    }
    if (cloud_xfyun_build_wss_uri(uri, 2048, XFYUN_TTS_HOST_STR, XFYUN_TTS_PATH) != ESP_OK) {
        ESP_LOGE(TAG, "build uri failed");
        free(uri);
        return ESP_FAIL;
    }

    char *req = build_request_json(utf8_text);
    if (!req) {
        free(uri);
        return ESP_ERR_NO_MEM;
    }

    xfyun_ctx_t ctx = {0};
    ctx.done_sem = xSemaphoreCreateBinary();
    ctx.request_json = req;
    ctx.err = ESP_OK;
    if (!ctx.done_sem) {
        free(uri);
        free(req);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 8192,
        .task_stack = 9216,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 60000,
        .disable_auto_reconnect = true,
    };

    ctx.client = esp_websocket_client_init(&ws_cfg);
    free(uri);
    uri = NULL;
    if (!ctx.client) {
        free(req);
        vSemaphoreDelete(ctx.done_sem);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_register_events(ctx.client, WEBSOCKET_EVENT_ANY, ws_handler, &ctx);
    esp_err_t st = esp_websocket_client_start(ctx.client);
    if (st != ESP_OK) {
        esp_websocket_client_destroy(ctx.client);
        free(req);
        vSemaphoreDelete(ctx.done_sem);
        return st;
    }

    if (!cloud_wait_sem_feed_wdt(ctx.done_sem, 120000)) {
        ESP_LOGE(TAG, "tts timeout");
        ctx.err = ESP_ERR_TIMEOUT;
    }

    esp_websocket_client_stop(ctx.client);
    esp_websocket_client_destroy(ctx.client);
    free(req);
    free(ctx.json_buf);
    vSemaphoreDelete(ctx.done_sem);

    if (ctx.err != ESP_OK) {
        free(ctx.pcm);
        return ctx.err;
    }
    if (ctx.pcm_len == 0) {
        free(ctx.pcm);
        return ESP_FAIL;
    }

    *out_pcm = ctx.pcm;
    *out_len = ctx.pcm_len;
    ESP_LOGI(TAG, "pcm bytes=%u", (unsigned)ctx.pcm_len);
    return ESP_OK;
}

esp_err_t xfyun_tts_synthesize_pcm_stream(const char *utf8_text,
                                          void (*on_pcm_chunk)(void *ctx, const uint8_t *pcm, size_t pcm_bytes),
                                          void *user_ctx)
{
    if (!utf8_text || !on_pcm_chunk) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(CONFIG_XFYUN_APP_ID) == 0 || strlen(CONFIG_XFYUN_API_KEY) == 0 ||
        strlen(CONFIG_XFYUN_API_SECRET) == 0) {
        ESP_LOGE(TAG, "XFYUN AppID/APIKey/APISecret not set");
        return ESP_ERR_INVALID_STATE;
    }

    char *uri = (char *)malloc(2048);
    if (!uri) {
        return ESP_ERR_NO_MEM;
    }
    if (cloud_xfyun_build_wss_uri(uri, 2048, XFYUN_TTS_HOST_STR, XFYUN_TTS_PATH) != ESP_OK) {
        ESP_LOGE(TAG, "build uri failed");
        free(uri);
        return ESP_FAIL;
    }

    char *req = build_request_json(utf8_text);
    if (!req) {
        free(uri);
        return ESP_ERR_NO_MEM;
    }

    xfyun_ctx_t ctx = {0};
    ctx.done_sem = xSemaphoreCreateBinary();
    ctx.request_json = req;
    ctx.err = ESP_OK;
    ctx.on_pcm_chunk = on_pcm_chunk;
    ctx.on_pcm_ctx = user_ctx;
    if (!ctx.done_sem) {
        free(uri);
        free(req);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 8192,
        .task_stack = 9216,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 60000,
        .disable_auto_reconnect = true,
    };

    ctx.client = esp_websocket_client_init(&ws_cfg);
    free(uri);
    uri = NULL;
    if (!ctx.client) {
        free(req);
        vSemaphoreDelete(ctx.done_sem);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_register_events(ctx.client, WEBSOCKET_EVENT_ANY, ws_handler, &ctx);
    esp_err_t st = esp_websocket_client_start(ctx.client);
    if (st != ESP_OK) {
        esp_websocket_client_destroy(ctx.client);
        free(req);
        vSemaphoreDelete(ctx.done_sem);
        return st;
    }

    if (!cloud_wait_sem_feed_wdt(ctx.done_sem, 120000)) {
        ESP_LOGE(TAG, "tts stream timeout");
        ctx.err = ESP_ERR_TIMEOUT;
    }

    esp_websocket_client_stop(ctx.client);
    esp_websocket_client_destroy(ctx.client);
    free(req);
    free(ctx.json_buf);
    vSemaphoreDelete(ctx.done_sem);

    return ctx.err != ESP_OK ? ctx.err : ESP_OK;
}
