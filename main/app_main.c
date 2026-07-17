#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_i2s.h"
#include "audio_pipeline.h"
#include "cloud_chain.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "ui_display.h"
#include "wake_sr.h"

static const char *TAG = "app";

static void delay_with_wdt_ms(int ms)
{
    while (ms > 0) {
        int slice = ms > 100 ? 100 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        esp_task_wdt_reset();
        ms -= slice;
    }
}

static void wait_for_network_ready(void)
{
    ui_display_set_state(UI_STATE_IDLE);
    ui_display_set_texts("网络连接中...", "请稍候");

    while (1) {
        esp_err_t err = net_wifi_init_and_connect();
        if (err == ESP_OK) {
            ui_display_set_state(UI_STATE_IDLE);
            ui_display_set_texts("网络已连接", "-");
            return;
        }

        ESP_LOGW(TAG, "wifi connect failed: %s", esp_err_to_name(err));
        ui_display_set_state(UI_STATE_ERROR);
        ui_display_set_texts("网络连接失败", esp_err_to_name(err));
        delay_with_wdt_ms(3000);
        ui_display_set_state(UI_STATE_IDLE);
        ui_display_set_texts("网络重试中...", "请检查 Wi-Fi");
    }
}

#if CONFIG_VOICE_ENERGY_FALLBACK
static float mic_buf_rms(const int16_t *s, int n)
{
    if (n <= 0) {
        return 0.f;
    }
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        double v = (double)s[i];
        acc += v * v;
    }
    return (float)sqrt(acc / (double)n);
}

static bool energy_gate_tick(int16_t *mic, int chunk_samples, int *loud_ms_acc)
{
    float rms = mic_buf_rms(mic, chunk_samples);
    int chunk_ms = chunk_samples * 1000 / 16000;
    if (chunk_ms < 1) {
        chunk_ms = 1;
    }
    if (rms >= (float)CONFIG_VOICE_ENERGY_RMS_MIN) {
        *loud_ms_acc += chunk_ms;
    } else {
        *loud_ms_acc -= chunk_ms * 2;
        if (*loud_ms_acc < 0) {
            *loud_ms_acc = 0;
        }
    }
    /* 持续 600ms 高于阈值才触发，避免一声短噪声（关门、敲桌、咳嗽）触发整轮 ASR */
    if (*loud_ms_acc >= 600) {
        *loud_ms_acc = 0;
        return true;
    }
    return false;
}
#endif

static void play_pcm_i2s(const int16_t *pcm, size_t sample_count)
{
    const size_t step = 512;
    /* 16kHz 下尾部静音；80 ms 不足以清空 I2S DMA + MAX98357，上拉到 500 ms
     * 再配合 audio_i2s_spk_stop() 关闭 TX 通道，彻底消除“最后一个字拖音”。 */
    const size_t tail_silence = (size_t)16000 * 500 / 1000;
    const int mic_resume_guard_ms = 30;
    static int16_t zeros[512];
    size_t done = 0;
    audio_i2s_set_capture_enabled(false);
    while (done < sample_count) {
        size_t n = sample_count - done;
        if (n > step) {
            n = step;
        }
        esp_err_t err = audio_i2s_spk_write(pcm + done, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker write failed: %s", esp_err_to_name(err));
            break;
        }
        done += n;
        esp_task_wdt_reset();
    }
    for (size_t tail_done = 0; tail_done < tail_silence; tail_done += step) {
        size_t n = tail_silence - tail_done;
        if (n > step) {
            n = step;
        }
        esp_err_t err = audio_i2s_spk_write(zeros, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker tail silence failed: %s", esp_err_to_name(err));
            break;
        }
        esp_task_wdt_reset();
    }
    audio_i2s_spk_stop();
    delay_with_wdt_ms(mic_resume_guard_ms);
    audio_i2s_set_capture_enabled(true);
}

static char s_stream_q_line[192];

static void chain_on_asr_show_q(const char *user_utf8, void *ctx)
{
    (void)ctx;
    char ql[192];
    snprintf(ql, sizeof(ql), "Q: %s", (user_utf8 && user_utf8[0]) ? user_utf8 : "-");
    snprintf(s_stream_q_line, sizeof(s_stream_q_line), "%s", ql);
    /* 答行用 ASCII 占位，避免「思考中」等字在字库外再出方框 */
    ui_display_set_texts(ql, "A: ...");
}

#if CONFIG_CHAIN_STREAMING && CONFIG_CLOUD_ASR_USE_XFYUN_WS && CONFIG_CLOUD_TTS_USE_XFYUN_WS
/* 流式 UI 刷新节流：每来一个 LLM token 就刷一次会把 SPI LCD 队列打爆
 * （lcd_panel.io.spi panel_io_spi_tx_color queue failed），
 * 同时抢 Core1 带宽导致 TTS 一段段。节流到 ~12 fps 足够观感。 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define UI_STREAM_MIN_INTERVAL_MS 150
static TickType_t s_last_answer_ui_tick;
static char s_last_answer_text[192];

static void chain_on_asr_stream(const char *user_utf8, void *ctx)
{
    chain_on_asr_show_q(user_utf8, ctx);
    ui_display_set_state(UI_STATE_THINK);
}

static void chain_on_asr_partial_ui(const char *asr_so_far, void *ctx)
{
    (void)ctx;
    if (!asr_so_far) {
        return;
    }
    char ql[192];
    snprintf(ql, sizeof(ql), "Q: %s", asr_so_far[0] ? asr_so_far : "-");
    snprintf(s_stream_q_line, sizeof(s_stream_q_line), "%s", ql);
    ui_display_set_texts(ql, "A: ...");
}

static void chain_on_answer_delta_ui(const char *answer_so_far, void *ctx)
{
    (void)ctx;
    char al[192];
    snprintf(al, sizeof(al), "A: %s", answer_so_far ? answer_so_far : "...");
    TickType_t now = xTaskGetTickCount();
    TickType_t dt = now - s_last_answer_ui_tick;
    if (dt < pdMS_TO_TICKS(UI_STREAM_MIN_INTERVAL_MS)) {
        /* 仍然缓存最新文本，最后一次 delta 肯定会被下一轮或 set_texts(Q,A) 覆盖显示 */
        snprintf(s_last_answer_text, sizeof(s_last_answer_text), "%s", al);
        return;
    }
    s_last_answer_ui_tick = now;
    snprintf(s_last_answer_text, sizeof(s_last_answer_text), "%s", al);
    ui_display_set_texts(s_stream_q_line[0] ? s_stream_q_line : "Q: -", al);
}

static void chain_on_first_spoken_pcm(void *ctx)
{
    (void)ctx;
    ui_display_set_state(UI_STATE_SPEAK);
}
#endif

void app_main(void)
{
    esp_task_wdt_config_t wcfg = {
        .timeout_ms = 120000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
#if CONFIG_ESP_TASK_WDT_INIT
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wcfg));
#else
    ESP_ERROR_CHECK(esp_task_wdt_init(&wcfg));
#endif
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    ESP_ERROR_CHECK(ui_display_init());
    esp_task_wdt_reset();

    wait_for_network_ready();
    esp_task_wdt_reset();

    ESP_ERROR_CHECK(audio_i2s_init());
    esp_task_wdt_reset();

    esp_err_t werr = wake_sr_init();
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "wake init failed: %s", esp_err_to_name(werr));
#if CONFIG_VOICE_ENERGY_FALLBACK
        ui_display_set_texts("唤醒: 模型未就绪", "可大声说话试试 / 烧 srmodels");
#else
        ui_display_set_texts("唤醒: 模型未就绪", "请 full flash 含 srmodels");
#endif
    }
    esp_task_wdt_reset();

    int chunk = wake_sr_feed_chunk_samples();
    int16_t *mic = (int16_t *)heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (!mic) {
        ESP_LOGE(TAG, "no mem for mic buf");
        ui_display_set_state(UI_STATE_ERROR);
        ui_display_set_texts("内存不足", "即将重启…");
        delay_with_wdt_ms(2000);
        esp_restart();
    }

    ui_display_set_state(UI_STATE_IDLE);

#if CONFIG_VOICE_ENERGY_FALLBACK
    int loud_ms_acc = 0;
#endif

    /* 开机冷启动期间 I2S DMA 通道、Wi-Fi 上电、模拟电源都还在 ramp，
     * 麦克风有较大瞬态噪声，容易让 energy gate 误触发"voice energy gate"。
     * 给一个 startup grace 期，期间只允许真正喊唤醒词触发，不接受 energy gate。 */
    const TickType_t startup_grace_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);

    while (1) {
        esp_task_wdt_reset();

        esp_err_t r = audio_i2s_mic_read(mic, (size_t)chunk);
        if (r != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool fire = false;
        /* 唤醒成功：仅在有有效 WakeNet 时检测唤醒词；音量门限作为补充（可不喊唤醒词，靠大声说话触发一轮） */
        if (werr == ESP_OK && wake_sr_process(mic)) {
            fire = true;
            ESP_LOGI(TAG, "wake word detected");
#if CONFIG_VOICE_ENERGY_FALLBACK
            loud_ms_acc = 0;
#endif
        }
#if CONFIG_VOICE_ENERGY_FALLBACK
        else if (xTaskGetTickCount() >= startup_grace_until &&
                 energy_gate_tick(mic, chunk, &loud_ms_acc)) {
            fire = true;
            ESP_LOGI(TAG, "voice energy gate");
        }
#endif

        if (!fire) {
#if !CONFIG_VOICE_ENERGY_FALLBACK
            if (werr != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
#endif
            continue;
        }

#if CONFIG_VOICE_ENERGY_FALLBACK
        loud_ms_acc = 0;
#endif

        ui_display_set_state(UI_STATE_LISTEN);

#if CONFIG_CHAIN_STREAMING && CONFIG_CLOUD_ASR_USE_XFYUN_WS && CONFIG_CLOUD_TTS_USE_XFYUN_WS
        cloud_chain_result_t out;
        memset(&out, 0, sizeof(out));
        cloud_chain_stream_opts_t sopts = {
            .on_asr = chain_on_asr_stream,
            .on_asr_ctx = NULL,
            .on_asr_partial = chain_on_asr_partial_ui,
            .on_asr_partial_ctx = NULL,
            .on_answer_delta = chain_on_answer_delta_ui,
            .on_answer_delta_ctx = NULL,
            .on_first_spoken_pcm = chain_on_first_spoken_pcm,
            .on_first_spoken_pcm_ctx = NULL,
        };
        esp_err_t cerr = cloud_chain_run_streaming(&sopts, &out);
#else
        uint8_t *pcm = NULL;
        size_t pcm_len = 0;
        if (audio_pipeline_record_until_silence_or_max(12000, &pcm, &pcm_len) != ESP_OK || !pcm) {
            ui_display_set_state(UI_STATE_ERROR);
            ui_display_set_texts("录音失败", "-");
            ui_display_set_state(UI_STATE_IDLE);
            continue;
        }
        uint8_t *wav = NULL;
        size_t wav_len = 0;
        if (audio_pcm_add_wav_header(pcm, pcm_len, &wav, &wav_len) != ESP_OK) {
            free(pcm);
            ui_display_set_state(UI_STATE_ERROR);
            ui_display_set_texts("-", "WAV 封装失败");
            ui_display_set_state(UI_STATE_IDLE);
            continue;
        }
        free(pcm);

        ui_display_set_state(UI_STATE_UPLOAD);
        vTaskDelay(pdMS_TO_TICKS(50));
        ui_display_set_state(UI_STATE_THINK);

        cloud_chain_result_t out;
        memset(&out, 0, sizeof(out));
        cloud_chain_opts_t copts = {
            .on_asr = chain_on_asr_show_q,
            .on_asr_ctx = NULL,
        };
        esp_err_t cerr = cloud_chain_run_ex(wav, wav_len, &out, &copts);
        free(wav);
#endif

        if (cerr != ESP_OK) {
            /* 用 ESP_EARLY_LOG 绕过 stdio 锁，避免罕见的链路失败叠加 stdio 异常再次 panic */
            ESP_EARLY_LOGE(TAG, "chain err %s", esp_err_to_name(cerr));
            ui_display_set_state(UI_STATE_ERROR);
            if (cerr == ESP_ERR_NOT_FOUND) {
                /* xfyun_iat：听写成功但 w 为空，非典型「云端失败」 */
                ui_display_set_texts("未识别到语音", "请大些声/靠近麦后重试");
            } else if (cerr == ESP_ERR_NOT_FINISHED) {
                /* record_stream: voice_ms < MIN_VOICE_MS，假唤醒/无人说话 */
                ui_display_set_texts("未检测到说话", "请唤醒后对准麦克风说话");
            } else if (cerr == ESP_ERR_INVALID_RESPONSE) {
                /* audio_pipeline：PCM 削顶饱和，软件增益过大或自激 */
                ui_display_set_texts("麦克风信号异常", "已削顶，请降低音量/远离喇叭");
            } else if (cerr == ESP_ERR_NOT_ALLOWED) {
                /* xfyun_iat: 10008 service instance invalid / 10200 read data timeout
                 * 讯飞服务繁忙或本地网络抖动导致发送过慢，等几秒重试通常就好 */
                ui_display_set_texts("讯飞服务繁忙", "请稍后再唤醒重试");
            } else {
                ui_display_set_texts("云端失败", esp_err_to_name(cerr));
            }
            cloud_chain_result_free(&out);
            ui_display_set_state(UI_STATE_IDLE);
            continue;
        }

        char ql[192];
        char al[192];
        snprintf(ql, sizeof(ql), "Q: %s", out.user_text ? out.user_text : "-");
        snprintf(al, sizeof(al), "A: %s", out.answer_text ? out.answer_text : "-");
        ui_display_set_texts(ql, al);

#if !(CONFIG_CHAIN_STREAMING && CONFIG_CLOUD_ASR_USE_XFYUN_WS && CONFIG_CLOUD_TTS_USE_XFYUN_WS)
        if (out.tts_pcm && out.tts_pcm_bytes >= 2) {
            ui_display_set_state(UI_STATE_SPEAK);
            size_t samples = out.tts_pcm_bytes / sizeof(int16_t);
            play_pcm_i2s((const int16_t *)out.tts_pcm, samples);
        } else {
            ui_display_set_state(UI_STATE_IDLE);
        }
#endif /* legacy TTS playback */
        cloud_chain_result_free(&out);
        ui_display_set_state(UI_STATE_IDLE);
    }
}
