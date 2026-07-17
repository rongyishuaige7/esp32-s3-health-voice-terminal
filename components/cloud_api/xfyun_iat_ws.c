/**
 * 讯飞开放平台「语音听写（流式版）」WebSocket API
 * https://www.xfyun.cn/doc/asr/voicedictation/API.html
 */
#include "xfyun_iat_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "cJSON.h"
#include "cloud_util.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

static const char *TAG = "xfyun_iat";

#define XFYUN_IAT_HOST_STR "iat-api.xfyun.cn"
#define XFYUN_IAT_PATH     "/v2/iat"
#define IAT_PCM_CHUNK      1280
/* 384KB = 12 秒 PCM @16kHz mono 16bit。必须 >= 录音上限(12s)+握手延迟(0~5s)，
 * 否则录音和 WS 并行时 ring 溢出会丢最旧 PCM —— 用户开始说话刚好赶上 WS 握手时段，
 * 那段最关键的语音正是被丢弃的。放在 PSRAM 上，不占内部 RAM。
 * 注意：录音侧 32KB/s，sender 侧也是 32KB/s（40ms/帧 × 1280B），两边速率完全匹配，
 * 所以 ring 占用稳态等于"握手延迟期间的积压量"。但保险起见按最坏情况配。 */
#define IAT_RING_CAP       (384 * 1024)

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t r;
    size_t w;
} iat_ring_t;

typedef struct {
    esp_websocket_client_handle_t client;
    SemaphoreHandle_t conn_sem;
    SemaphoreHandle_t done_sem;
    SemaphoreHandle_t sender_done_sem;
    SemaphoreHandle_t data_sem;
    TaskHandle_t sender_task;
    SemaphoreHandle_t ring_mux;
    iat_ring_t ring;
    bool pcm_eof;
    char *text_acc;
    char *frag_json;
    size_t frag_len;
    size_t frag_cap;
    esp_err_t err;
    volatile bool finished;
    bool conn_gave;
    void (*on_partial)(const char *full_text_utf8, void *user);
    void *on_partial_ctx;
} iat_stream_ctx_t;

static size_t ring_used(iat_ring_t *rb)
{
    if (rb->w >= rb->r) {
        return rb->w - rb->r;
    }
    return rb->cap - rb->r + rb->w;
}

static size_t ring_free(iat_ring_t *rb)
{
    return (rb->cap - 1) - ring_used(rb);
}

static esp_err_t ring_write(iat_ring_t *rb, const uint8_t *p, size_t n)
{
    if (n >= rb->cap) {
        /* 单次写入比整个环还大，无法容纳，丢弃最旧的，只保留最后 cap-1 */
        p += (n - (rb->cap - 1));
        n = rb->cap - 1;
    }
    /* 若空间不足：丢弃最旧数据（移动读指针），保证实时流不卡死。
     * 这在 WS 握手慢/网络抖动时尤其重要，否则会让 record_stream 拿到
     * ESP_ERR_NO_MEM 提前中止，用户表现为唤醒后立刻报错。 */
    size_t avail = ring_free(rb);
    if (n > avail) {
        size_t drop = n - avail;
        rb->r = (rb->r + drop) % rb->cap;
    }
    for (size_t i = 0; i < n; i++) {
        rb->data[rb->w] = p[i];
        rb->w = (rb->w + 1) % rb->cap;
    }
    return ESP_OK;
}

static void ring_read_consume(iat_ring_t *rb, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = rb->data[rb->r];
        rb->r = (rb->r + 1) % rb->cap;
    }
}

static esp_err_t append_text(iat_stream_ctx_t *ctx, const char *s);

static esp_err_t append_best_cw_text(iat_stream_ctx_t *ctx, cJSON *cw)
{
    if (!cJSON_IsArray(cw) || cJSON_GetArraySize(cw) <= 0) {
        return ESP_OK;
    }

    cJSON *best = cJSON_GetArrayItem(cw, 0);
    cJSON *w = best ? cJSON_GetObjectItem(best, "w") : NULL;
    if (!cJSON_IsString(w) || !w->valuestring) {
        return ESP_OK;
    }
    return append_text(ctx, w->valuestring);
}

static esp_err_t append_text(iat_stream_ctx_t *ctx, const char *s)
{
    if (!s || !*s) {
        return ESP_OK;
    }
    size_t add = strlen(s);
    size_t old = ctx->text_acc ? strlen(ctx->text_acc) : 0;
    char *nb = (char *)realloc(ctx->text_acc, old + add + 1);
    if (!nb) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(nb + old, s, add + 1);
    ctx->text_acc = nb;
    if (ctx->on_partial && ctx->text_acc) {
        ctx->on_partial(ctx->text_acc, ctx->on_partial_ctx);
    }
    return ESP_OK;
}

static void parse_iat_json(iat_stream_ctx_t *ctx, const char *json_str)
{
    if (!json_str || !*json_str) {
        return;
    }
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    int code_val = 0;
    if (cJSON_IsNumber(code)) {
        code_val = (int)code->valuedouble;
    } else if (cJSON_IsString(code) && code->valuestring) {
        code_val = atoi(code->valuestring);
    }
    if (code != NULL && code_val != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "iat err code=%d msg=%s", code_val,
                 cJSON_IsString(msg) ? msg->valuestring : "?");
        if (code_val == 11200) {
            ESP_LOGE(TAG, "11200=未授权能力点：检查控制台是否开通听写；domain 若为 medical 等须单独授权，可改为 iat");
        }
        /* 10008 service instance invalid: 讯飞服务容量不足/瞬时不可用，提示用户重试。
         * 10165 client frame out of order: 客户端帧乱序（首帧 status 错误等），代码 bug。
         * 10114 session timeout: 会话超过 60 秒。
         * 10200 read data timeout: 客户端 10 秒未发送数据，多因发送 task 卡住。
         * 11200 unauthorized: 套餐/能力未授权。 */
        if (code_val == 10008 || code_val == 10200) {
            ctx->err = ESP_ERR_NOT_ALLOWED;  /* 服务繁忙/网络抖动，让上层提示用户重试 */
        } else {
            ctx->err = ESP_FAIL;
        }
        ctx->finished = true;
        cJSON_Delete(root);
        xSemaphoreGive(ctx->done_sem);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data && cJSON_IsObject(data)) {
        cJSON *result = cJSON_GetObjectItem(data, "result");
        if (result && cJSON_IsObject(result)) {
            cJSON *ws = cJSON_GetObjectItem(result, "ws");
            if (cJSON_IsArray(ws)) {
                int n = cJSON_GetArraySize(ws);
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(ws, i);
                    cJSON *cw = cJSON_GetObjectItem(item, "cw");
                    if (!cJSON_IsArray(cw)) {
                        continue;
                    }
                    if (append_best_cw_text(ctx, cw) != ESP_OK) {
                        ctx->err = ESP_ERR_NO_MEM;
                        ctx->finished = true;
                        cJSON_Delete(root);
                        xSemaphoreGive(ctx->done_sem);
                        return;
                    }
                }
            }
        }
        cJSON *st = cJSON_GetObjectItem(data, "status");
        if (cJSON_IsNumber(st) && (int)st->valuedouble == 2) {
            if (!ctx->text_acc || ctx->text_acc[0] == '\0') {
                char *dbg = cJSON_PrintUnformatted(root);
                if (dbg) {
                    ESP_LOGW(TAG, "听写终包无文本(多因麦克风/I2S 声道)： %.400s%s",
                             dbg, strlen(dbg) > 400 ? "..." : "");
                    free(dbg);
                }
            }
            ctx->finished = true;
            cJSON_Delete(root);
            xSemaphoreGive(ctx->done_sem);
            return;
        }
    }

    cJSON_Delete(root);
}

static void json_buf_append(iat_stream_ctx_t *ctx, const char *chunk, int chunk_len)
{
    if (chunk_len <= 0) {
        return;
    }
    size_t need = ctx->frag_len + (size_t)chunk_len + 1;
    if (need > ctx->frag_cap) {
        size_t ncap = ctx->frag_cap ? ctx->frag_cap * 2 : 4096;
        while (ncap < need) {
            ncap *= 2;
        }
        char *nb = realloc(ctx->frag_json, ncap);
        if (!nb) {
            ctx->err = ESP_ERR_NO_MEM;
            return;
        }
        ctx->frag_json = nb;
        ctx->frag_cap = ncap;
    }
    memcpy(ctx->frag_json + ctx->frag_len, chunk, (size_t)chunk_len);
    ctx->frag_len += (size_t)chunk_len;
    ctx->frag_json[ctx->frag_len] = '\0';
}

static void ws_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    iat_stream_ctx_t *ctx = (iat_stream_ctx_t *)handler_args;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        if (!ctx->conn_gave) {
            ctx->conn_gave = true;
            xSemaphoreGive(ctx->conn_sem);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev->data_ptr && ev->data_len > 0 &&
            (ev->op_code == WS_TRANSPORT_OPCODES_TEXT || ev->op_code == WS_TRANSPORT_OPCODES_CONT)) {
            json_buf_append(ctx, ev->data_ptr, ev->data_len);
            if (ctx->err != ESP_OK) {
                break;
            }
            bool frame_done = (ev->payload_len > 0) &&
                              (ev->payload_offset + ev->data_len >= ev->payload_len);
            if (frame_done && ev->fin && ctx->frag_json) {
                parse_iat_json(ctx, ctx->frag_json);
                ctx->frag_len = 0;
                ctx->frag_json[0] = '\0';
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
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

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (!ctx->finished) {
            ESP_LOGW(TAG, "disconnected before iat done");
            ctx->err = ESP_ERR_INVALID_STATE;
            ctx->finished = true;
            xSemaphoreGive(ctx->done_sem);
        }
        break;

    default:
        break;
    }
}

static char *b64_pcm(const uint8_t *pcm, size_t n)
{
    size_t b64_len = 4 * ((n + 2) / 3) + 8;
    char *b64 = (char *)malloc(b64_len + 1);
    if (!b64) {
        return NULL;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, b64_len + 1, &olen, pcm, n) != 0) {
        free(b64);
        return NULL;
    }
    b64[olen] = '\0';
    return b64;
}

static char *build_first_frame(const uint8_t *pcm_chunk, size_t chunk_len)
{
    char *ab64 = b64_pcm(pcm_chunk, chunk_len);
    if (!ab64) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *common = cJSON_CreateObject();
    cJSON *business = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !common || !business || !data) {
        free(ab64);
        cJSON_Delete(data);
        cJSON_Delete(business);
        cJSON_Delete(common);
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(common, "app_id", CONFIG_XFYUN_APP_ID);
    cJSON_AddItemToObject(root, "common", common);

    cJSON_AddStringToObject(business, "language", CONFIG_XFYUN_IAT_LANGUAGE);
    cJSON_AddStringToObject(business, "domain", CONFIG_XFYUN_IAT_DOMAIN);
    cJSON_AddStringToObject(business, "accent", CONFIG_XFYUN_IAT_ACCENT);
    cJSON_AddNumberToObject(business, "eos", 10000);
    cJSON_AddItemToObject(root, "business", business);

    cJSON_AddNumberToObject(data, "status", 0);
    cJSON_AddStringToObject(data, "format", "audio/L16;rate=16000");
    cJSON_AddStringToObject(data, "encoding", "raw");
    cJSON_AddStringToObject(data, "audio", ab64);
    cJSON_AddItemToObject(root, "data", data);

    free(ab64);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static char *build_mid_frame(const uint8_t *pcm_chunk, size_t chunk_len)
{
    char *ab64 = b64_pcm(pcm_chunk, chunk_len);
    if (!ab64) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        free(ab64);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNumberToObject(data, "status", 1);
    cJSON_AddStringToObject(data, "format", "audio/L16;rate=16000");
    cJSON_AddStringToObject(data, "encoding", "raw");
    cJSON_AddStringToObject(data, "audio", ab64);
    cJSON_AddItemToObject(root, "data", data);
    free(ab64);
    return cJSON_PrintUnformatted(root);
}

static void iat_stream_send_task(void *arg)
{
    iat_stream_ctx_t *ctx = (iat_stream_ctx_t *)arg;

    /* 必须先等 WS 握手完成。但不能等录音完才发 —— 讯飞要求每帧间隔 40ms，
     * 整个会话 60s 上限、超过 10s 不发数据会断连。所以策略是：
     * 1) 等 conn_sem（WS connected）
     * 2) 严格 40ms/帧 1280B 节流发送（讯飞文档强制要求）
     * 3) ring 在 PSRAM 上 384KB（12 秒）FIFO；录音 32KB/s 与 sender 32KB/s 速率匹配，
     *    握手期间 ring 会积压最多 ~160KB（5 秒），仍 < 384KB 不会溢出
     */
    if (xSemaphoreTake(ctx->conn_sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "wait connect timeout");
        ctx->err = ESP_ERR_TIMEOUT;
        ctx->finished = true;
        xSemaphoreGive(ctx->done_sem);
        goto sender_exit;
    }

    bool first = true;
    uint8_t tmp[IAT_PCM_CHUNK];
    const TickType_t frame_interval = pdMS_TO_TICKS(40);
    TickType_t last_send_tick = 0;
    int frames_sent = 0;

    while (ctx->err == ESP_OK && !ctx->finished) {
        size_t used = 0;
        if (xSemaphoreTake(ctx->ring_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ctx->err = ESP_FAIL;
            break;
        }
        used = ring_used(&ctx->ring);
        bool eof = ctx->pcm_eof;
        xSemaphoreGive(ctx->ring_mux);

        if (used < IAT_PCM_CHUNK) {
            /* 不足一帧：若 EOF 已置且 used==0 直接跳出；若 EOF 但还有残留则补零发完最后一帧；
             * 否则等录音再写入。 */
            if (eof && used == 0) {
                break;
            }
            if (eof) {
                if (xSemaphoreTake(ctx->ring_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
                    ctx->err = ESP_FAIL;
                    break;
                }
                memset(tmp, 0, IAT_PCM_CHUNK);
                ring_read_consume(&ctx->ring, tmp, used);
                xSemaphoreGive(ctx->ring_mux);
                /* 已读到 tmp，跳过下面的常规读取 */
            } else {
                if (!cloud_wait_sem_feed_wdt(ctx->data_sem, 12000)) {
                    ESP_LOGE(TAG, "wait pcm data timeout");
                    ctx->err = ESP_ERR_TIMEOUT;
                    break;
                }
                continue;
            }
        } else {
            if (xSemaphoreTake(ctx->ring_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
                ctx->err = ESP_FAIL;
                break;
            }
            ring_read_consume(&ctx->ring, tmp, IAT_PCM_CHUNK);
            xSemaphoreGive(ctx->ring_mux);
        }

        TickType_t t_build_begin = xTaskGetTickCount();
        char *json = first ? build_first_frame(tmp, IAT_PCM_CHUNK)
                           : build_mid_frame(tmp, IAT_PCM_CHUNK);
        if (!json) {
            ctx->err = ESP_ERR_NO_MEM;
            break;
        }
        TickType_t t_build_end = xTaskGetTickCount();
        /* 严格 40ms 节流。讯飞文档明确：发太快服务端会"识别有误"。
         * 即使 ring 有积压，也不要快发——积压的代价是丢最旧 PCM，比丢首帧 status=0 标志好。 */
        if (!first) {
            TickType_t now = xTaskGetTickCount();
            TickType_t elapsed = now - last_send_tick;
            if (elapsed < frame_interval) {
                vTaskDelay(frame_interval - elapsed);
            }
        }
        TickType_t t_send_begin = xTaskGetTickCount();
        if (esp_websocket_client_send_text(ctx->client, json, strlen(json), pdMS_TO_TICKS(15000)) < 0) {
            ESP_LOGE(TAG, "send_text failed");
            free(json);
            ctx->err = ESP_FAIL;
            break;
        }
        TickType_t t_send_end = xTaskGetTickCount();
        last_send_tick = t_send_end;
        free(json);
        first = false;
        frames_sent++;
        /* 每 25 帧 (~1秒) 打一次 timing 日志，帮助定位 send 慢 */
        if (frames_sent == 1 || frames_sent % 25 == 0) {
            ESP_LOGI(TAG, "frame#%d build=%ums send=%ums", frames_sent,
                     (unsigned)((t_build_end - t_build_begin) * portTICK_PERIOD_MS),
                     (unsigned)((t_send_end - t_send_begin) * portTICK_PERIOD_MS));
        }
    }
    ESP_LOGI(TAG, "sender done, %d frames sent", frames_sent);

    if (ctx->err == ESP_OK && !ctx->finished) {
        const char *end = "{\"data\":{\"status\":2}}";
        if (esp_websocket_client_send_text(ctx->client, end, strlen(end), pdMS_TO_TICKS(10000)) < 0) {
            ESP_LOGE(TAG, "send end failed");
            ctx->err = ESP_FAIL;
        }
    }

    if (ctx->err != ESP_OK && !ctx->finished) {
        ctx->finished = true;
        xSemaphoreGive(ctx->done_sem);
    }

sender_exit:
    xSemaphoreGive(ctx->sender_done_sem);
    vTaskDelete(NULL);
}

static const uint8_t *strip_wav_pcm(const uint8_t *wav, size_t wav_len, size_t *out_pcm_len)
{
    if (wav_len >= 12 && memcmp(wav, "RIFF", 4) == 0 && memcmp(wav + 8, "WAVE", 4) == 0) {
        if (wav_len < 44) {
            *out_pcm_len = 0;
            return wav;
        }
        *out_pcm_len = wav_len - 44;
        return wav + 44;
    }
    *out_pcm_len = wav_len;
    return wav;
}

static void iat_stream_destroy_inner(iat_stream_ctx_t *ctx, bool wait_sender)
{
    if (!ctx) {
        return;
    }
    if (wait_sender && ctx->sender_task && ctx->sender_done_sem) {
        if (!cloud_wait_sem_feed_wdt(ctx->sender_done_sem, 45000)) {
            ESP_LOGW(TAG, "iat_send task exit timeout");
            ctx->finished = true;
            esp_websocket_client_stop(ctx->client);
            if (!cloud_wait_sem_feed_wdt(ctx->sender_done_sem, 20000)) {
                if (ctx->sender_task) {
                    vTaskDelete(ctx->sender_task);
                    ctx->sender_task = NULL;
                }
            }
        }
    }
    if (ctx->client) {
        esp_websocket_client_stop(ctx->client);
        esp_websocket_client_destroy(ctx->client);
        ctx->client = NULL;
    }
    if (ctx->conn_sem) {
        vSemaphoreDelete(ctx->conn_sem);
        ctx->conn_sem = NULL;
    }
    if (ctx->done_sem) {
        vSemaphoreDelete(ctx->done_sem);
        ctx->done_sem = NULL;
    }
    if (ctx->sender_done_sem) {
        vSemaphoreDelete(ctx->sender_done_sem);
        ctx->sender_done_sem = NULL;
    }
    if (ctx->data_sem) {
        vSemaphoreDelete(ctx->data_sem);
        ctx->data_sem = NULL;
    }
    if (ctx->ring_mux) {
        vSemaphoreDelete(ctx->ring_mux);
        ctx->ring_mux = NULL;
    }
    free(ctx->text_acc);
    ctx->text_acc = NULL;
    free(ctx->ring.data);
    ctx->ring.data = NULL;
    free(ctx->frag_json);
    ctx->frag_json = NULL;
    free(ctx);
}

esp_err_t xfyun_iat_stream_begin(xfyun_iat_stream_t **out_ctx, void (*on_partial)(const char *full_text_utf8, void *user),
                                 void *user)
{
    if (!out_ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ctx = NULL;

    if (strlen(CONFIG_XFYUN_APP_ID) == 0 || strlen(CONFIG_XFYUN_API_KEY) == 0 ||
        strlen(CONFIG_XFYUN_API_SECRET) == 0) {
        ESP_LOGE(TAG, "XFYUN AppID/APIKey/APISecret not set");
        return ESP_ERR_INVALID_STATE;
    }

    iat_stream_ctx_t *ctx = (iat_stream_ctx_t *)calloc(1, sizeof(iat_stream_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    ctx->ring.data = (uint8_t *)heap_caps_malloc(IAT_RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->ring.data) {
        ctx->ring.data = (uint8_t *)malloc(IAT_RING_CAP);
    }
    if (!ctx->ring.data) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    ctx->ring.cap = IAT_RING_CAP;
    ctx->ring.r = 0;
    ctx->ring.w = 0;
    ctx->on_partial = on_partial;
    ctx->on_partial_ctx = user;

    ctx->conn_sem = xSemaphoreCreateBinary();
    ctx->done_sem = xSemaphoreCreateBinary();
    ctx->sender_done_sem = xSemaphoreCreateBinary();
    ctx->data_sem = xSemaphoreCreateBinary();
    ctx->ring_mux = xSemaphoreCreateMutex();
    if (!ctx->conn_sem || !ctx->done_sem || !ctx->sender_done_sem || !ctx->data_sem || !ctx->ring_mux) {
        iat_stream_destroy_inner(ctx, false);
        return ESP_ERR_NO_MEM;
    }

    char *uri = (char *)malloc(2048);
    if (!uri) {
        iat_stream_destroy_inner(ctx, false);
        return ESP_ERR_NO_MEM;
    }
    if (cloud_xfyun_build_wss_uri(uri, 2048, XFYUN_IAT_HOST_STR, XFYUN_IAT_PATH) != ESP_OK) {
        free(uri);
        iat_stream_destroy_inner(ctx, false);
        return ESP_FAIL;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 8192,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 30000,
        .reconnect_timeout_ms = 5000,
        .disable_auto_reconnect = true,
    };

    ctx->client = esp_websocket_client_init(&ws_cfg);
    free(uri);
    uri = NULL;
    if (!ctx->client) {
        iat_stream_destroy_inner(ctx, false);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_register_events(ctx->client, WEBSOCKET_EVENT_ANY, ws_handler, ctx);
    esp_err_t st = esp_websocket_client_start(ctx->client);
    if (st != ESP_OK) {
        esp_websocket_client_destroy(ctx->client);
        ctx->client = NULL;
        iat_stream_destroy_inner(ctx, false);
        return st;
    }

    /* 不阻塞等 WS 连接：录音和握手必须并行。ring 缓冲 128KB（8 秒）足以覆盖握手延迟，
     * sender task 连好后会 40ms 节流追发缓存。若握手 >8 秒（极罕见），ring 丢最旧数据
     * （唤醒词残响），比阻塞录音丢用户全部语音好得多。 */

    if (xTaskCreate(iat_stream_send_task, "iat_send", 12288, ctx, 5, &ctx->sender_task) != pdPASS) {
        esp_websocket_client_stop(ctx->client);
        esp_websocket_client_destroy(ctx->client);
        ctx->client = NULL;
        iat_stream_destroy_inner(ctx, false);
        return ESP_ERR_NO_MEM;
    }

    *out_ctx = (xfyun_iat_stream_t *)ctx;
    return ESP_OK;
}

esp_err_t xfyun_iat_stream_feed(xfyun_iat_stream_t *s, const uint8_t *pcm, size_t len)
{
    iat_stream_ctx_t *ctx = (iat_stream_ctx_t *)s;
    if (!ctx || (!pcm && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (xSemaphoreTake(ctx->ring_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t e = ring_write(&ctx->ring, pcm, len);
    xSemaphoreGive(ctx->ring_mux);
    if (e != ESP_OK) {
        return e;
    }
    xSemaphoreGive(ctx->data_sem);
    return ESP_OK;
}

void xfyun_iat_stream_mark_pcm_done(xfyun_iat_stream_t *s)
{
    iat_stream_ctx_t *ctx = (iat_stream_ctx_t *)s;
    if (!ctx) {
        return;
    }
    if (xSemaphoreTake(ctx->ring_mux, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ctx->pcm_eof = true;
        xSemaphoreGive(ctx->ring_mux);
    }
    xSemaphoreGive(ctx->data_sem);
}

esp_err_t xfyun_iat_stream_finish(xfyun_iat_stream_t *s, char **out_text)
{
    iat_stream_ctx_t *ctx = (iat_stream_ctx_t *)s;
    if (!ctx || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    if (!cloud_wait_sem_feed_wdt(ctx->done_sem, 120000)) {
        ESP_LOGE(TAG, "iat timeout");
        ctx->err = ESP_ERR_TIMEOUT;
    }

    if (!cloud_wait_sem_feed_wdt(ctx->sender_done_sem, 45000)) {
        ESP_LOGW(TAG, "iat_send task exit timeout");
        ctx->finished = true;
        esp_websocket_client_stop(ctx->client);
        if (!cloud_wait_sem_feed_wdt(ctx->sender_done_sem, 20000) && ctx->sender_task) {
            vTaskDelete(ctx->sender_task);
            ctx->sender_task = NULL;
        }
    }

    esp_websocket_client_stop(ctx->client);
    esp_websocket_client_destroy(ctx->client);
    ctx->client = NULL;
    vSemaphoreDelete(ctx->conn_sem);
    ctx->conn_sem = NULL;
    vSemaphoreDelete(ctx->done_sem);
    ctx->done_sem = NULL;
    vSemaphoreDelete(ctx->sender_done_sem);
    ctx->sender_done_sem = NULL;
    vSemaphoreDelete(ctx->data_sem);
    ctx->data_sem = NULL;
    vSemaphoreDelete(ctx->ring_mux);
    ctx->ring_mux = NULL;
    free(ctx->ring.data);
    ctx->ring.data = NULL;
    free(ctx->frag_json);
    ctx->frag_json = NULL;

    esp_err_t ret = ESP_OK;
    char *text_out = NULL;
    if (ctx->err != ESP_OK) {
        free(ctx->text_acc);
        ret = ctx->err;
    } else if (!ctx->text_acc || strlen(ctx->text_acc) == 0) {
        /* 服务端 code=0 但终包 w 为空：多为音量/底噪/声道问题，与云端宕机区分 */
        ESP_LOGW(TAG, "听写无有效文本(返回 ESP_ERR_NOT_FOUND 给上层展示「未识别到语音」)");
        free(ctx->text_acc);
        ret = ESP_ERR_NOT_FOUND;
    } else {
        text_out = ctx->text_acc;
    }
    free(ctx);

    if (ret != ESP_OK) {
        return ret;
    }
    *out_text = text_out;
    ESP_LOGI(TAG, "iat text len=%u", (unsigned)strlen(*out_text));
    return ESP_OK;
}

void xfyun_iat_stream_abort(xfyun_iat_stream_t *s)
{
    iat_stream_ctx_t *ctx = (iat_stream_ctx_t *)s;
    if (!ctx) {
        return;
    }
    ctx->finished = true;
    ctx->err = ESP_ERR_INVALID_STATE;
    if (ctx->client) {
        esp_websocket_client_stop(ctx->client);
    }
    xSemaphoreGive(ctx->done_sem);
    xSemaphoreGive(ctx->data_sem);
}

esp_err_t xfyun_iat_transcribe_wav(const uint8_t *wav, size_t wav_len, char **out_text)
{
    if (!wav || !out_text || wav_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    size_t pcm_len = 0;
    const uint8_t *pcm = strip_wav_pcm(wav, wav_len, &pcm_len);
    if (pcm_len < 320) {
        ESP_LOGE(TAG, "pcm too short");
        return ESP_ERR_INVALID_ARG;
    }

    xfyun_iat_stream_t *st = NULL;
    esp_err_t err = xfyun_iat_stream_begin(&st, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t off = 0; off < pcm_len && err == ESP_OK; off += IAT_PCM_CHUNK) {
        size_t n = pcm_len - off;
        if (n > IAT_PCM_CHUNK) {
            n = IAT_PCM_CHUNK;
        }
        err = xfyun_iat_stream_feed(st, pcm + off, n);
    }
    if (err == ESP_OK) {
        xfyun_iat_stream_mark_pcm_done(st);
        err = xfyun_iat_stream_finish(st, out_text);
    } else {
        esp_err_t fe = err;
        xfyun_iat_stream_abort(st);
        iat_stream_destroy_inner((iat_stream_ctx_t *)st, true);
        return fe;
    }
    return err;
}
