#include "cloud_chain.h"
#include "cloud_asr.h"
#include "cloud_deepseek.h"
#include "cloud_tts.h"
#include "medical_preprocess.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_CHAIN_STREAMING && CONFIG_CLOUD_ASR_USE_XFYUN_WS && CONFIG_CLOUD_TTS_USE_XFYUN_WS
#include "audio_pipeline.h"
#include "cloud_stream_pipe.h"
#include "xfyun_iat_ws.h"
#endif

static const char *TAG = "cloud_chain";

/** 剥除 DeepSeek reasoner 等模型返回的推理块，避免整段上屏/播报 */
static void strip_tag_pair_inplace(char *s, const char *open, const char *close)
{
    if (!s || !open || !close) {
        return;
    }
    const size_t open_len = strlen(open);
    const size_t close_len = strlen(close);
    for (;;) {
        char *p = strstr(s, open);
        if (!p) {
            break;
        }
        char *q = strstr(p + open_len, close);
        if (!q) {
            break;
        }
        q += close_len;
        memmove(p, q, strlen(q) + 1);
    }
}

static void strip_llm_reasoning_blocks(char *s)
{
    if (!s) {
        return;
    }
    strip_tag_pair_inplace(s, "<redacted_thinking>", "</redacted_thinking>");
    strip_tag_pair_inplace(s, "<Thinking>", "</Thinking>");
}

void cloud_chain_result_free(cloud_chain_result_t *r)
{
    if (!r) {
        return;
    }
    free(r->user_text);
    free(r->answer_text);
    free(r->tts_pcm);
    r->user_text = NULL;
    r->answer_text = NULL;
    r->tts_pcm = NULL;
    r->tts_pcm_bytes = 0;
}

esp_err_t cloud_chain_run_ex(const uint8_t *wav, size_t wav_len, cloud_chain_result_t *out,
                             const cloud_chain_opts_t *opts)
{
    if (!wav || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char *asr_text = NULL;
    esp_err_t err = cloud_asr_transcribe_wav(wav, wav_len, &asr_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ASR failed: %s", esp_err_to_name(err));
        return err;
    }
    out->user_text = medical_normalize_query(asr_text);
    free(asr_text);
    if (!out->user_text) {
        return ESP_ERR_NO_MEM;
    }

    if (opts && opts->on_asr) {
        opts->on_asr(out->user_text, opts->on_asr_ctx);
    }

    err = cloud_deepseek_chat(medical_system_prompt(), out->user_text, &out->answer_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DeepSeek failed: %s", esp_err_to_name(err));
        free(out->user_text);
        out->user_text = NULL;
        return err;
    }

    if (out->answer_text) {
        strip_llm_reasoning_blocks(out->answer_text);
    }

    err = cloud_tts_synthesize(out->answer_text, &out->tts_pcm, &out->tts_pcm_bytes);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "TTS not configured, text only");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t cloud_chain_run(const uint8_t *wav, size_t wav_len, cloud_chain_result_t *out)
{
    return cloud_chain_run_ex(wav, wav_len, out, NULL);
}

#if CONFIG_CHAIN_STREAMING && CONFIG_CLOUD_ASR_USE_XFYUN_WS && CONFIG_CLOUD_TTS_USE_XFYUN_WS

typedef struct {
    const cloud_chain_stream_opts_t *sopts;
    char acc[8192];
} chain_stream_ctx_t;

static esp_err_t feed_iat_stream(void *ctx, const int16_t *pcm, size_t samples)
{
    xfyun_iat_stream_t *st = (xfyun_iat_stream_t *)ctx;
    return xfyun_iat_stream_feed(st, (const uint8_t *)pcm, samples * sizeof(int16_t));
}

static void iat_partial_cb(const char *full, void *user)
{
    chain_stream_ctx_t *c = (chain_stream_ctx_t *)user;
    if (c->sopts && c->sopts->on_asr_partial && full) {
        c->sopts->on_asr_partial(full, c->sopts->on_asr_partial_ctx);
    }
}

static void on_llm_sentence(char *sentence, void *p)
{
    (void)p;
    /* 整段 TTS 模式：每句不再单独发到讯飞 TTS（每句都要重建 WS+DNS+TLS，
     * 中间 DNS 抖动经常 getaddrinfo 失败导致中间漏播），而是 LLM 流完成后
     * 一次性把整段文本送 TTS。这里收到的句子在 on_llm_delta 累积的 acc 里也有，
     * 直接释放即可。 */
    free(sentence);
}

static void on_llm_delta(const char *so_far, void *p)
{
    chain_stream_ctx_t *c = (chain_stream_ctx_t *)p;
    if (!so_far) {
        return;
    }
    strncpy(c->acc, so_far, sizeof(c->acc) - 1);
    c->acc[sizeof(c->acc) - 1] = '\0';
    if (c->sopts && c->sopts->on_answer_delta) {
        c->sopts->on_answer_delta(c->acc, c->sopts->on_answer_delta_ctx);
    }
}

esp_err_t cloud_chain_run_streaming(const cloud_chain_stream_opts_t *opts, cloud_chain_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    esp_err_t er = cloud_stream_pipe_init();
    if (er != ESP_OK) {
        return er;
    }

    chain_stream_ctx_t iat_ctx = {.sopts = opts, .acc = {0}};
    xfyun_iat_stream_t *iat = NULL;
    er = xfyun_iat_stream_begin(&iat, iat_partial_cb, &iat_ctx);
    if (er != ESP_OK) {
        return er;
    }

    er = audio_pipeline_record_stream(feed_iat_stream, iat, 12000);
    if (er != ESP_OK) {
        xfyun_iat_stream_abort(iat);
        char *dummy = NULL;
        xfyun_iat_stream_finish(iat, &dummy);
        free(dummy);
        return er;
    }
    xfyun_iat_stream_mark_pcm_done(iat);

    char *raw_asr = NULL;
    er = xfyun_iat_stream_finish(iat, &raw_asr);
    if (er != ESP_OK) {
        ESP_LOGE(TAG, "ASR stream failed: %s", esp_err_to_name(er));
        return er;
    }

    out->user_text = medical_normalize_query(raw_asr);
    free(raw_asr);
    if (!out->user_text) {
        return ESP_ERR_NO_MEM;
    }

    if (opts && opts->on_asr) {
        opts->on_asr(out->user_text, opts->on_asr_ctx);
    }

    chain_stream_ctx_t llm_ctx = {.sopts = opts, .acc = {0}};
    cloud_stream_pipe_prepare_playback(opts ? opts->on_first_spoken_pcm : NULL,
                                       opts ? opts->on_first_spoken_pcm_ctx : NULL);

    er = cloud_deepseek_chat_stream(medical_system_prompt(), out->user_text, on_llm_sentence, on_llm_delta,
                                    &llm_ctx);

    if (er != ESP_OK) {
        cloud_stream_pipe_mark_llm_sentences_done();
        ESP_LOGE(TAG, "DeepSeek stream failed: %s", esp_err_to_name(er));
        free(out->user_text);
        out->user_text = NULL;
        return er;
    }

    strip_llm_reasoning_blocks(llm_ctx.acc);
    out->answer_text = strdup(llm_ctx.acc);
    if (!out->answer_text) {
        cloud_stream_pipe_mark_llm_sentences_done();
        free(out->user_text);
        out->user_text = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 整段 TTS：LLM 完成后把累积的回答整段送讯飞 TTS，避免每句一个 WS 连接
     * 频繁触发 DNS/TLS 失败和句间断句卡顿。 */
    if (out->answer_text[0] != '\0') {
        char *whole = strdup(out->answer_text);
        if (whole) {
            if (cloud_stream_pipe_enqueue_sentence(whole) != ESP_OK) {
                free(whole);
            }
        }
    }
    cloud_stream_pipe_mark_llm_sentences_done();

    er = cloud_stream_pipe_wait_playback_done(180000);
    if (er != ESP_OK) {
        ESP_LOGW(TAG, "playback wait: %s", esp_err_to_name(er));
    }
    return ESP_OK;
}

#endif
