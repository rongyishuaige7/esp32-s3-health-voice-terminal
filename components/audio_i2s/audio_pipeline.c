#include "audio_pipeline.h"
#include "audio_i2s.h"
#include "board_pins.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "audio_pipe";

#define CHUNK_SAMPLES   512
/* 600ms 静默→停录。中文换气一般 < 400ms，超过 600ms 多半真的说完了。
 * 太长（如 900ms）会让讯飞的 EOS=10s 不能保护我们：用户说完后还要等 0.9s 再发完
 * 整个剩余 ring，会话总时长可能突破讯飞 60s 上限或 10s 不发数据上限。 */
#define SILENCE_MS      600
/* SILENCE_RMS / VOICE_RMS 形成迟滞：rms < 1500 累计 silence；rms >= 2200 清零 silence。
 * 1500 ↔ 2200 之间维持当前状态。这样环境底噪（典型 800-1200 RMS）即使偶尔越过 1500
 * 也不会触发 silence 清零，避免"用户停了但永远停不下来"。 */
#define SILENCE_RMS     1500.0f
#define WARMUP_CHUNKS   6
/* 判定"真的有人说话"的门槛 */
#define VOICE_RMS       2200.0f
#define MIN_VOICE_MS    300

static void apply_mic_gain(int16_t *s, size_t n)
{
#if BOARD_MIC_PCM_GAIN_NUM != BOARD_MIC_PCM_GAIN_DEN
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)s[i] * (int32_t)BOARD_MIC_PCM_GAIN_NUM / (int32_t)BOARD_MIC_PCM_GAIN_DEN;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        s[i] = (int16_t)v;
    }
#else
    (void)s;
    (void)n;
#endif
}

static float compute_rms(const int16_t *s, size_t n)
{
    if (n == 0) {
        return 0;
    }
    double acc = 0;
    for (size_t i = 0; i < n; i++) {
        double v = (double)s[i];
        acc += v * v;
    }
    return (float)sqrt(acc / (double)n);
}

esp_err_t audio_pipeline_record_start(void)
{
    return ESP_OK;
}

void audio_pipeline_record_abort(void)
{
}

esp_err_t audio_pcm_add_wav_header(const uint8_t *pcm, size_t pcm_len,
                                   uint8_t **out_wav, size_t *out_wav_len)
{
    if (!pcm || !out_wav || !out_wav_len) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t hdr = 44;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(hdr + pcm_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)malloc(hdr + pcm_len);
    }
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t sample_rate = 16000;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t byte_rate = sample_rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;
    uint32_t riff_payload = 36 + (uint32_t)pcm_len;

    memcpy(buf + 0, "RIFF", 4);
    memcpy(buf + 4, &riff_payload, 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t sub1 = 16;
    memcpy(buf + 16, &sub1, 4);
    uint16_t audio_fmt = 1;
    memcpy(buf + 20, &audio_fmt, 2);
    memcpy(buf + 22, &channels, 2);
    memcpy(buf + 24, &sample_rate, 4);
    memcpy(buf + 28, &byte_rate, 4);
    memcpy(buf + 32, &block_align, 2);
    memcpy(buf + 34, &bits, 2);
    memcpy(buf + 36, "data", 4);
    uint32_t sub2 = (uint32_t)pcm_len;
    memcpy(buf + 40, &sub2, 4);
    memcpy(buf + 44, pcm, pcm_len);
    *out_wav = buf;
    *out_wav_len = hdr + pcm_len;
    return ESP_OK;
}

esp_err_t audio_pipeline_record_stream(audio_pipeline_feed_cb feed_cb, void *ctx, int max_ms)
{
    if (!feed_cb) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t sample_rate = 16000;
    const size_t max_samples = (size_t)sample_rate * (size_t)max_ms / 1000;
    int16_t chunk[CHUNK_SAMPLES];
    size_t total_samples = 0;
    int silence_run_ms = 0;
    int16_t peak_abs = 0;
    float max_rms = 0.0f;
    int voice_ms = 0;

    for (int w = 0; w < WARMUP_CHUNKS; w++) {
        (void)audio_i2s_mic_read(chunk, CHUNK_SAMPLES);
    }

    while (total_samples < max_samples) {
        size_t need = CHUNK_SAMPLES;
        if (total_samples + need > max_samples) {
            need = max_samples - total_samples;
        }
        esp_err_t err = audio_i2s_mic_read(chunk, need);
        if (err != ESP_OK) {
            return err;
        }
        apply_mic_gain(chunk, need);
        err = feed_cb(ctx, chunk, need);
        if (err != ESP_OK) {
            return err;
        }
        total_samples += need;

        float rms = compute_rms(chunk, need);
        if (rms > max_rms) {
            max_rms = rms;
        }
        for (size_t i = 0; i < need; i++) {
            int16_t a = chunk[i];
            if (a < 0) {
                a = (int16_t)-(int32_t)a;
            }
            if (a > peak_abs) {
                peak_abs = a;
            }
        }

        int chunk_ms = (int)(need * 1000 / sample_rate);
        if (rms >= VOICE_RMS) {
            voice_ms += chunk_ms;
        }
        /* 迟滞 VAD：低于 SILENCE_RMS 才累加 silence；高于 VOICE_RMS 才清零；
         * 中间灰区维持当前状态，避免底噪反复触发清零。 */
        if (rms < SILENCE_RMS) {
            silence_run_ms += chunk_ms;
            if (silence_run_ms >= SILENCE_MS && total_samples > sample_rate / 2) {
                break;
            }
        } else if (rms >= VOICE_RMS) {
            silence_run_ms = 0;
        }
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }

    /* peak 很小(例如 <500) 或 max_rms <~300 往往说明 I2S 声道错(slot_mask)、
     * L/R 接线错、或 INMP441 VDD/GND 没焊好 —— 讯飞会返回空 w */
    ESP_LOGI(TAG, "record_stream done, samples=%u peak=%d max_rms=%.0f voice_ms=%d",
             (unsigned)total_samples, (int)peak_abs, max_rms, voice_ms);
    /* peak 顶到 32767 且 rms 也逼近满量程：几乎每个 sample 都被削顶，
     * 不是有效语音（常见于软件增益过大或 MAX98357 回授耦合），讯飞必然返回空 w。
     * 直接上报采集异常，避免浪费一次 ~5 秒的 IAT 往返。 */
    if (peak_abs >= 32760 && max_rms >= 20000.0f) {
        ESP_LOGW(TAG, "mic saturated（疑似增益过大/自激），建议降 BOARD_MIC_PCM_GAIN_NUM");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (voice_ms < MIN_VOICE_MS) {
        /* 假唤醒 / 无人说话：让上层跳过 ASR 上传而不是当成真错误 */
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

esp_err_t audio_pipeline_record_until_silence_or_max(int max_ms, uint8_t **out_pcm, size_t *out_bytes)
{
    if (!out_pcm || !out_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_pcm = NULL;
    *out_bytes = 0;

    const size_t sample_rate = 16000;
    const size_t max_samples = (size_t)sample_rate * (size_t)max_ms / 1000;
    size_t cap = max_samples * sizeof(int16_t);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)malloc(cap);
    }
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    int16_t chunk[CHUNK_SAMPLES];
    size_t total_samples = 0;
    int silence_run_ms = 0;

    /* 丢弃刚进入录音时的 I2S/DMA 不稳定段，避免首段静音/ garbage 拉低识别 */
    for (int w = 0; w < WARMUP_CHUNKS; w++) {
        (void)audio_i2s_mic_read(chunk, CHUNK_SAMPLES);
    }

    while (total_samples < max_samples) {
        size_t need = CHUNK_SAMPLES;
        if (total_samples + need > max_samples) {
            need = max_samples - total_samples;
        }
        esp_err_t err = audio_i2s_mic_read(chunk, need);
        if (err != ESP_OK) {
            free(buf);
            return err;
        }
        apply_mic_gain(chunk, need);
        memcpy(buf + total_samples * sizeof(int16_t), chunk, need * sizeof(int16_t));
        total_samples += need;

        float rms = compute_rms(chunk, need);
        int chunk_ms = (int)(need * 1000 / sample_rate);
        if (rms < SILENCE_RMS) {
            silence_run_ms += chunk_ms;
            if (silence_run_ms >= SILENCE_MS && total_samples > sample_rate / 2) {
                break;
            }
        } else {
            silence_run_ms = 0;
        }
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }

    *out_bytes = total_samples * sizeof(int16_t);
    *out_pcm = buf;
    if (total_samples > 0) {
        size_t head = total_samples < 8000 ? total_samples : 8000;
        ESP_LOGI(TAG, "recorded %u bytes PCM, rms(首0.5s)=%.0f",
                 (unsigned)*out_bytes, compute_rms((const int16_t *)buf, head));
    } else {
        ESP_LOGI(TAG, "recorded 0 bytes PCM");
    }
    return ESP_OK;
}
