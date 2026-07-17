#include "cloud_deepseek.h"
#include "cloud_util.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

static const char *TAG = "deepseek";

static esp_http_client_handle_t s_stream_client;

static void strip_stream_tags_inplace(char *s)
{
    if (!s) {
        return;
    }
    for (;;) {
        char *p = strstr(s, "<think>");
        if (!p) {
            break;
        }
        char *q = strstr(p, "</think>");
        if (!q) {
            break;
        }
        q += strlen("</think>");
        memmove(p, q, strlen(q) + 1);
    }
}

static esp_err_t ds_ensure_stream_client(void)
{
    if (s_stream_client) {
        return ESP_OK;
    }
    char url[320];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", CONFIG_DEEPSEEK_API_BASE);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_stream_client = esp_http_client_init(&cfg);
    if (!s_stream_client) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void ds_close_stream_client(void)
{
    if (s_stream_client) {
        esp_http_client_close(s_stream_client);
    }
}

esp_err_t cloud_deepseek_chat(const char *system_prompt, const char *user_message,
                              char **out_reply)
{
    if (!user_message || !out_reply) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_reply = NULL;
    if (strlen(CONFIG_DEEPSEEK_API_KEY) == 0) {
        ESP_LOGW(TAG, "no DEEPSEEK_API_KEY, skip LLM");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", CONFIG_DEEPSEEK_MODEL);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    if (!msgs) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (system_prompt && strlen(system_prompt) > 0) {
        cJSON *m0 = cJSON_CreateObject();
        if (!m0) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(m0, "role", "system");
        cJSON_AddStringToObject(m0, "content", system_prompt);
        cJSON_AddItemToArray(msgs, m0);
    }
    cJSON *m1 = cJSON_CreateObject();
    if (!m1) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(m1, "role", "user");
    cJSON_AddStringToObject(m1, "content", user_message);
    cJSON_AddItemToArray(msgs, m1);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", CONFIG_DEEPSEEK_API_BASE);

    char *resp = NULL;
    esp_err_t err = cloud_http_post_json_capture(url, CONFIG_DEEPSEEK_API_KEY, json, &resp);
    free(json);
    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    cJSON *ans = cJSON_Parse(resp);
    free(resp);
    if (!ans) {
        return ESP_FAIL;
    }
    cJSON *choices = cJSON_GetObjectItem(ans, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    const char *txt = cJSON_IsString(content) ? content->valuestring : NULL;
    if (!txt) {
        cJSON_Delete(ans);
        return ESP_FAIL;
    }
    *out_reply = strdup(txt);
    cJSON_Delete(ans);
    return *out_reply ? ESP_OK : ESP_ERR_NO_MEM;
}

static size_t utf8_count_prefix(const char *s, size_t nbytes)
{
    size_t n = 0;
    for (size_t i = 0; i < nbytes && s[i];) {
        unsigned char c = (unsigned char)s[i];
        size_t adv = 1;
        if (c >= 0xf0) {
            adv = 4;
        } else if (c >= 0xe0) {
            adv = 3;
        } else if (c >= 0xc0) {
            adv = 2;
        }
        if (i + adv > nbytes) {
            break;
        }
        n++;
        i += adv;
    }
    return n;
}

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

/** 找最早出现的强断句标点，返回结束位置（字节偏移，不含后续），未找到返回 SIZE_MAX */
static size_t find_strong_break(const char *buf, size_t len)
{
    size_t best = SIZE_MAX;
    static const char *const strong[] = {
        "\n", "。", "！", "？", "；", ".", "!", "?", ";",
    };
    for (size_t k = 0; k < sizeof(strong) / sizeof(strong[0]); k++) {
        const char *p = buf;
        size_t sl = strlen(strong[k]);
        while (len >= sl) {
            const char *q = strstr(p, strong[k]);
            if (!q) {
                break;
            }
            size_t end = (size_t)(q - buf) + sl;
            if (end < best) {
                best = end;
            }
            p = q + 1;
        }
    }
    return best;
}

static size_t find_weak_break(const char *buf, size_t len)
{
    size_t best = SIZE_MAX;
    static const char *const weak[] = { "，", ",", "、" };
    for (size_t k = 0; k < sizeof(weak) / sizeof(weak[0]); k++) {
        const char *p = buf;
        size_t sl = strlen(weak[k]);
        while (len >= sl) {
            const char *q = strstr(p, weak[k]);
            if (!q) {
                break;
            }
            size_t end = (size_t)(q - buf) + sl;
            if (end < best) {
                best = end;
            }
            p = q + 1;
        }
    }
    return best;
}

static esp_err_t flush_pending(char **pending, size_t *pend_len, cloud_deepseek_sentence_cb on_sentence, void *uctx,
                               bool force)
{
    if (!pending || !*pending || *pend_len == 0) {
        return ESP_OK;
    }
    char *buf = *pending;
    size_t len = *pend_len;

    while (len > 0) {
        size_t cut = find_strong_break(buf, len);
        if (cut == SIZE_MAX) {
            cut = find_weak_break(buf, len);
            if (cut != SIZE_MAX && utf8_count_prefix(buf, cut) < 6) {
                cut = SIZE_MAX;
            }
        }
        if (cut == SIZE_MAX) {
            if (force) {
                cut = len;
            } else if (len > 120) {
                cut = 120;
                while (cut > 0 && ((unsigned char)buf[cut] & 0xc0) == 0x80) {
                    cut--;
                }
            } else {
                break;
            }
        }

        char *sent = malloc(cut + 1);
        if (!sent) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(sent, buf, cut);
        sent[cut] = '\0';
        while (cut > 0 && isspace((unsigned char)sent[cut - 1])) {
            sent[--cut] = '\0';
        }
        if (sent[0]) {
            on_sentence(sent, uctx);
        } else {
            free(sent);
        }

        size_t rest = len - cut;
        if (rest > 0) {
            memmove(buf, buf + cut, rest + 1);
            len = rest;
        } else {
            buf[0] = '\0';
            len = 0;
        }
        *pend_len = len;
    }
    return ESP_OK;
}

esp_err_t cloud_deepseek_chat_stream(const char *system_prompt, const char *user_message,
                                     cloud_deepseek_sentence_cb on_sentence,
                                     void (*on_full_delta)(const char *answer_so_far, void *ctx), void *ctx)
{
    if (!user_message || !on_sentence) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(CONFIG_DEEPSEEK_API_KEY) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t er = ds_ensure_stream_client();
    if (er != ESP_OK) {
        return er;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", CONFIG_DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    if (!msgs) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (system_prompt && strlen(system_prompt) > 0) {
        cJSON *m0 = cJSON_CreateObject();
        cJSON_AddStringToObject(m0, "role", "system");
        cJSON_AddStringToObject(m0, "content", system_prompt);
        cJSON_AddItemToArray(msgs, m0);
    }
    cJSON *m1 = cJSON_CreateObject();
    cJSON_AddStringToObject(m1, "role", "user");
    cJSON_AddStringToObject(m1, "content", user_message);
    cJSON_AddItemToArray(msgs, m1);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", CONFIG_DEEPSEEK_API_BASE);

    esp_http_client_set_url(s_stream_client, url);
    esp_http_client_set_method(s_stream_client, HTTP_METHOD_POST);
    esp_http_client_set_header(s_stream_client, "Content-Type", "application/json");
    char auth[280];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_DEEPSEEK_API_KEY);
    esp_http_client_set_header(s_stream_client, "Authorization", auth);

    int body_len = (int)strlen(json);
    er = esp_http_client_open(s_stream_client, body_len);
    if (er != ESP_OK) {
        free(json);
        ds_close_stream_client();
        return er;
    }
    int w = esp_http_client_write(s_stream_client, json, body_len);
    free(json);
    if (w != body_len) {
        esp_http_client_close(s_stream_client);
        return ESP_FAIL;
    }

    int64_t hdr = esp_http_client_fetch_headers(s_stream_client);
    (void)hdr;
    int code = esp_http_client_get_status_code(s_stream_client);
    if (code < 200 || code >= 300) {
        ESP_LOGE(TAG, "stream http status %d", code);
        esp_http_client_close(s_stream_client);
        return ESP_FAIL;
    }

    char *acc = calloc(1, 8192);
    if (!acc) {
        esp_http_client_close(s_stream_client);
        return ESP_ERR_NO_MEM;
    }
    size_t acc_len = 0;

    char *pending = calloc(1, 4096);
    size_t pend_len = 0;
    if (!pending) {
        free(acc);
        esp_http_client_close(s_stream_client);
        return ESP_ERR_NO_MEM;
    }

    char *raw = (char *)malloc(1024);
    char *line = (char *)malloc(2048);
    if (!raw || !line) {
        free(raw);
        free(line);
        free(acc);
        free(pending);
        esp_http_client_close(s_stream_client);
        return ESP_ERR_NO_MEM;
    }
    size_t line_len = 0;

    int r;
    while ((r = esp_http_client_read(s_stream_client, raw, 1024)) > 0) {
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
        for (int i = 0; i < r; i++) {
            char c = raw[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len >= 6 && memcmp(line, "data: ", 6) == 0) {
                    const char *payload = line + 6;
                    if (strcmp(payload, "[DONE]") == 0) {
                        line_len = 0;
                        goto stream_done;
                    }
                    cJSON *ev = cJSON_Parse(payload);
                    if (ev) {
                        cJSON *ch = cJSON_GetObjectItem(ev, "choices");
                        cJSON *first = cJSON_IsArray(ch) ? cJSON_GetArrayItem(ch, 0) : NULL;
                        cJSON *delta = first ? cJSON_GetObjectItem(first, "delta") : NULL;
                        cJSON *content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
                        const char *piece = cJSON_IsString(content) ? content->valuestring : NULL;
                        if (piece && piece[0]) {
                            size_t pl = strlen(piece);
                            size_t need = acc_len + pl + 1;
                            if (need > 8192) {
                                /* 丢弃过旧前缀以控内存 */
                                size_t drop = need - 4096;
                                memmove(acc, acc + drop, acc_len - drop + 1);
                                acc_len -= drop;
                            }
                            memcpy(acc + acc_len, piece, pl + 1);
                            acc_len += pl;
                            strip_stream_tags_inplace(acc);
                            acc_len = strlen(acc);
                            if (on_full_delta) {
                                on_full_delta(acc, ctx);
                            }
                            size_t need_sz = pend_len + pl + 1;
                            if (need_sz > 4096) {
                                char *nb = realloc(pending, need_sz + 512);
                                if (!nb) {
                                    cJSON_Delete(ev);
                                    free(raw);
                                    free(line);
                                    free(acc);
                                    free(pending);
                                    esp_http_client_close(s_stream_client);
                                    return ESP_ERR_NO_MEM;
                                }
                                pending = nb;
                            }
                            memcpy(pending + pend_len, piece, pl);
                            pend_len += pl;
                            pending[pend_len] = '\0';
                            flush_pending(&pending, &pend_len, on_sentence, ctx, false);
                        }
                        cJSON_Delete(ev);
                    }
                }
                line_len = 0;
            } else if (line_len + 1 < 2048) {
                line[line_len++] = c;
            } else {
                line_len = 0;
            }
        }
    }

stream_done:
    flush_pending(&pending, &pend_len, on_sentence, ctx, true);
    free(raw);
    free(line);
    free(pending);
    free(acc);
    esp_http_client_close(s_stream_client);
    return ESP_OK;
}
