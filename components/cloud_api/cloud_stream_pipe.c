#include "cloud_stream_pipe.h"

#include <stdlib.h>
#include <string.h>

#include "audio_i2s.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_CLOUD_TTS_USE_XFYUN_WS
#include "xfyun_tts_ws.h"
#endif

static const char *TAG = "stream_pipe";

#define PIPE_BIT_TTS_DONE   BIT0

#define PCM_STREAM_BYTES    (64 * 1024)
#define TTS_QUEUE_DEPTH     12

static StreamBufferHandle_t s_pcm_sb;
static StaticStreamBuffer_t s_pcm_sb_ctrl;
static uint8_t *s_pcm_sb_storage;
static QueueHandle_t s_tts_q;
static StaticQueue_t s_tts_q_ctrl;
static uint8_t s_tts_q_storage[TTS_QUEUE_DEPTH * sizeof(char *)];
static EventGroupHandle_t s_ev;
static StaticEventGroup_t s_ev_ctrl;
static TaskHandle_t s_tts_task;
static TaskHandle_t s_spk_task;
static SemaphoreHandle_t s_init_mx;
static StaticSemaphore_t s_init_mx_ctrl;
static bool s_inited;

static void (*s_on_first_pcm)(void *ctx);
static void *s_on_first_ctx;
static volatile bool s_first_pcm;
static volatile bool s_spk_keepalive;

static void tts_on_pcm(void *user, const uint8_t *pcm, size_t pcm_bytes)
{
    (void)user;
    if (pcm_bytes == 0) {
        return;
    }
    if (!s_first_pcm) {
        s_first_pcm = true;
        s_spk_keepalive = true;
        if (s_on_first_pcm) {
            s_on_first_pcm(s_on_first_ctx);
        }
    }
    size_t sent = 0;
    while (sent < pcm_bytes) {
        size_t n = xStreamBufferSend(s_pcm_sb, pcm + sent, pcm_bytes - sent, pdMS_TO_TICKS(5000));
        if (n == 0) {
            ESP_LOGW(TAG, "pcm stream buffer full");
            break;
        }
        sent += n;
    }
}

static void tts_pipe_task(void *arg)
{
    (void)arg;
#if CONFIG_CLOUD_TTS_USE_XFYUN_WS
    for (;;) {
        char *sent = NULL;
        if (xQueueReceive(s_tts_q, &sent, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (sent == NULL) {
            xEventGroupSetBits(s_ev, PIPE_BIT_TTS_DONE);
            continue;
        }
        esp_err_t te = xfyun_tts_synthesize_pcm_stream(sent, tts_on_pcm, NULL);
        if (te != ESP_OK) {
            ESP_LOGW(TAG, "tts sentence failed: %s", esp_err_to_name(te));
        }
        free(sent);
    }
#else
    vTaskDelete(NULL);
#endif
}

static uint8_t s_spk_buf[2048];

static void spk_pipe_task(void *arg)
{
    (void)arg;
    /* 句间静音帧：当 TTS 还在合成下一句、stream buffer 暂时没数据时，
     * 主动给 I2S 喂 0，防止 DMA underrun 导致 MAX98357 重放最后一帧 PCM
     * （表现为"最后一个字连续叠音 5 次"）。 */
    static const int16_t silence[256] = {0};
    for (;;) {
        size_t n = xStreamBufferReceive(s_pcm_sb, s_spk_buf, sizeof(s_spk_buf),
                                        pdMS_TO_TICKS(20));
        if (n > 0) {
            size_t samples = n / sizeof(int16_t);
            if (samples > 0) {
                audio_i2s_spk_write((const int16_t *)s_spk_buf, samples);
            }
        } else if (s_spk_keepalive) {
            /* 已开始播放且整段尚未结束：补一小段静音，避免 DMA underrun
             * 导致最后一帧 PCM 被硬件循环重放（叠音）。 */
            audio_i2s_spk_write(silence, sizeof(silence) / sizeof(silence[0]));
        }
    }
}

esp_err_t cloud_stream_pipe_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (!s_init_mx) {
        /* 互斥锁也用 static，避免任何动态分配分到 SPIRAM */
        s_init_mx = xSemaphoreCreateMutexStatic(&s_init_mx_ctrl);
    }
    if (!s_init_mx) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_init_mx, portMAX_DELAY);

    if (s_inited) {
        xSemaphoreGive(s_init_mx);
        return ESP_OK;
    }

    /* 存储区只做 memcpy，可放 SPIRAM；控制块都用 static（.bss 内部 DRAM） */
    if (!s_pcm_sb_storage) {
        s_pcm_sb_storage = (uint8_t *)heap_caps_malloc(PCM_STREAM_BYTES + 1,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm_sb_storage) {
            s_pcm_sb_storage = (uint8_t *)heap_caps_malloc(PCM_STREAM_BYTES + 1,
                                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    ESP_EARLY_LOGI(TAG, "heap: int=%u spi=%u largest_int=%u largest_spi=%u storage=%p",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                   (void *)s_pcm_sb_storage);

    if (s_pcm_sb_storage) {
        s_pcm_sb = xStreamBufferCreateStatic(PCM_STREAM_BYTES + 1, 256,
                                             s_pcm_sb_storage, &s_pcm_sb_ctrl);
    }
    s_tts_q = xQueueCreateStatic(TTS_QUEUE_DEPTH, sizeof(char *),
                                 s_tts_q_storage, &s_tts_q_ctrl);
    s_ev = xEventGroupCreateStatic(&s_ev_ctrl);

    if (!s_pcm_sb || !s_tts_q || !s_ev) {
        s_pcm_sb = NULL;
        if (s_pcm_sb_storage) {
            heap_caps_free(s_pcm_sb_storage);
            s_pcm_sb_storage = NULL;
        }
        s_tts_q = NULL;
        s_ev = NULL;
        xSemaphoreGive(s_init_mx);
        ESP_EARLY_LOGE(TAG, "alloc stream pipe failed");
        return ESP_ERR_NO_MEM;
    }

    /* 任务栈必须在内部 DRAM，且绑到 Core1 避开 Core0 上 LVGL/Wi-Fi/WakeNet 抢栈 */
    BaseType_t r1 = xTaskCreatePinnedToCore(tts_pipe_task, "tts_pipe", 5120, NULL, 6,
                                            &s_tts_task, 1);
    if (r1 != pdPASS) {
        s_pcm_sb = NULL; s_tts_q = NULL; s_ev = NULL;
        if (s_pcm_sb_storage) { heap_caps_free(s_pcm_sb_storage); s_pcm_sb_storage = NULL; }
        xSemaphoreGive(s_init_mx);
        ESP_EARLY_LOGE(TAG, "create tts task failed");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t r2 = xTaskCreatePinnedToCore(spk_pipe_task, "spk_pipe", 4096, NULL, 5,
                                            &s_spk_task, 1);
    if (r2 != pdPASS) {
        vTaskDelete(s_tts_task); s_tts_task = NULL;
        s_pcm_sb = NULL; s_tts_q = NULL; s_ev = NULL;
        if (s_pcm_sb_storage) { heap_caps_free(s_pcm_sb_storage); s_pcm_sb_storage = NULL; }
        xSemaphoreGive(s_init_mx);
        ESP_EARLY_LOGE(TAG, "create spk task failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_EARLY_LOGI(TAG, "tts_task=%p spk_task=%p", s_tts_task, s_spk_task);

    s_inited = true;
    xSemaphoreGive(s_init_mx);
    ESP_EARLY_LOGI(TAG, "stream pipe ready");
    return ESP_OK;
}

void cloud_stream_pipe_prepare_playback(void (*on_first_pcm)(void *ctx), void *ctx)
{
    s_on_first_pcm = on_first_pcm;
    s_on_first_ctx = ctx;
    s_first_pcm = false;
    s_spk_keepalive = false;
    xEventGroupClearBits(s_ev, PIPE_BIT_TTS_DONE);
    uint8_t drain[256];
    while (xStreamBufferReceive(s_pcm_sb, drain, sizeof(drain), 0) > 0) {
    }
    xStreamBufferReset(s_pcm_sb);
    audio_i2s_set_capture_enabled(false);
}

esp_err_t cloud_stream_pipe_enqueue_sentence(char *sentence_utf8)
{
    if (!sentence_utf8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(s_tts_q, &sentence_utf8, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void cloud_stream_pipe_mark_llm_sentences_done(void)
{
    char *eof = NULL;
    if (xQueueSend(s_tts_q, &eof, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "queue eof failed");
    }
}

esp_err_t cloud_stream_pipe_wait_playback_done(uint32_t timeout_ms)
{
    const TickType_t t0 = xTaskGetTickCount();
    for (;;) {
        EventBits_t b = xEventGroupGetBits(s_ev);
        if ((b & PIPE_BIT_TTS_DONE) && xStreamBufferBytesAvailable(s_pcm_sb) == 0) {
            /* 整段回答结束，停止 spk_pipe_task 继续写静音，
             * 否则下面 spk_stop 后又会被 write 自动重新 enable */
            s_spk_keepalive = false;
            vTaskDelay(pdMS_TO_TICKS(80));
            break;
        }
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(timeout_ms)) {
            audio_i2s_set_capture_enabled(true);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }
    /* 尾静音：必须大于 I2S DMA 全部 buffer + MAX98357 模拟衰减，
     * 否则 DMA 没吃到新数据时会把最后一帧样本无限重放 → “最后一个字拖音” */
    const int16_t z[512] = {0};
    /* 16 × 512 samples = 8192 samples ≈ 512 ms @16 kHz */
    for (int i = 0; i < 16; i++) {
        audio_i2s_spk_write(z, 512);
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }
    /* 真正的关键：disable TX 通道，让 DMA 停下来；下次 write 会自动 enable */
    audio_i2s_spk_stop();
    vTaskDelay(pdMS_TO_TICKS(30));
    audio_i2s_set_capture_enabled(true);
    return ESP_OK;
}
