#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "cloud_util";

void cloud_format_rfc1123_gmt(char *buf, size_t sz, time_t t)
{
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(buf, sz, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             wd[tm_utc.tm_wday], tm_utc.tm_mday, mon[tm_utc.tm_mon],
             tm_utc.tm_year + 1900, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

int cloud_hmac_sha256_b64_sig(const char *secret, const char *sign_in, char *sig_b64, size_t sig_b64_sz)
{
    unsigned char mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, (const unsigned char *)secret, strlen(secret),
                        (const unsigned char *)sign_in, strlen(sign_in), mac) != 0) {
        return -1;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)sig_b64, sig_b64_sz, &olen, mac, sizeof(mac)) != 0) {
        return -1;
    }
    return 0;
}

void cloud_url_encode_component(const char *in, char *out, size_t out_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0xf];
        }
    }
    out[j] = '\0';
}

bool cloud_wait_sem_feed_wdt(SemaphoreHandle_t sem, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    const TickType_t slice = pdMS_TO_TICKS(4000);
    while ((xTaskGetTickCount() - start) < limit) {
        /* 只有在当前任务已订阅 WDT 的情况下喂狗，避免跨任务调用时刷 "task not found" 日志 */
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
        const TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t remain = limit - elapsed;
        if (remain > slice) {
            remain = slice;
        }
        if (xSemaphoreTake(sem, remain) == pdTRUE) {
            return true;
        }
    }
    return false;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} buf_t;

static esp_err_t buf_append(buf_t *b, const void *data, size_t add, bool nul_terminate)
{
    size_t extra = nul_terminate ? 1 : 0;
    if (b->len + add + extra > b->cap) {
        size_t ncap = (b->cap ? b->cap : 512) * 2;
        while (ncap < b->len + add + extra) {
            ncap *= 2;
        }
        uint8_t *nb = realloc(b->data, ncap);
        if (!nb) {
            return ESP_ERR_NO_MEM;
        }
        b->data = nb;
        b->cap = ncap;
    }
    if (add > 0 && data) {
        memcpy(b->data + b->len, data, add);
        b->len += add;
    }
    if (nul_terminate) {
        b->data[b->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_client_write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int written = esp_http_client_write(client, data + off, (int)(len - off));
        if (written < 0) {
            return ESP_FAIL;
        }
        off += (size_t)written;
    }
    return ESP_OK;
}

static esp_err_t http_read_response_text(esp_http_client_handle_t client, buf_t *buf)
{
    char scratch[1024];
    int empty_reads = 0;

    while (1) {
        int n = esp_http_client_read(client, scratch, sizeof(scratch));
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (++empty_reads >= 12) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        empty_reads = 0;
        ESP_RETURN_ON_ERROR(buf_append(buf, scratch, (size_t)n, true), TAG, "append http resp");
    }

    if (!buf->data) {
        ESP_RETURN_ON_ERROR(buf_append(buf, "", 0, true), TAG, "term empty resp");
    }
    return ESP_OK;
}

static char *dup_http_header_value(esp_http_client_handle_t client, const char *key)
{
    char *value = NULL;
    if (esp_http_client_get_header(client, key, &value) != ESP_OK || !value) {
        return NULL;
    }
    return strdup(value);
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    buf_t *b = (buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b) {
        return buf_append(b, evt->data, (size_t)evt->data_len, true);
    }
    return ESP_OK;
}

esp_err_t cloud_http_post_json_capture(const char *url, const char *auth_bearer,
                                       const char *json_body, char **out_response)
{
    if (!url || !json_body || !out_response) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_response = NULL;

    buf_t buf = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
        .event_handler = on_http_event,
        .user_data = &buf,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (auth_bearer && strlen(auth_bearer) > 0) {
        char hdr[512];
        snprintf(hdr, sizeof(hdr), "Bearer %s", auth_bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    esp_http_client_set_post_field(c, json_body, strlen(json_body));
    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        free(buf.data);
        ESP_LOGE(TAG, "http err %s", esp_err_to_name(err));
        return err;
    }
    if (code < 200 || code >= 300) {
        ESP_LOGE(TAG, "http status %d (response body suppressed)", code);
        free(buf.data);
        return ESP_FAIL;
    }
    *out_response = (char *)buf.data;
    return ESP_OK;
}

static esp_err_t on_http_binary_event(esp_http_client_event_t *evt)
{
    buf_t *b = (buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && evt->data_len > 0) {
        return buf_append(b, evt->data, (size_t)evt->data_len, false);
    }
    return ESP_OK;
}

esp_err_t cloud_http_post_binary_response(const char *url, const char *auth_bearer,
                                        const char *json_body,
                                        uint8_t **out_data, size_t *out_len,
                                        char **out_content_type)
{
    if (!url || !out_data || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;
    if (out_content_type) {
        *out_content_type = NULL;
    }

    buf_t buf = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
        .event_handler = on_http_binary_event,
        .user_data = &buf,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (auth_bearer && strlen(auth_bearer) > 0) {
        char hdr[512];
        snprintf(hdr, sizeof(hdr), "Bearer %s", auth_bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    if (json_body && strlen(json_body) > 0) {
        esp_http_client_set_post_field(c, json_body, strlen(json_body));
    } else {
        esp_http_client_set_post_field(c, "", 0);
    }
    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        free(buf.data);
        return err;
    }
    if (code < 200 || code >= 300) {
        esp_http_client_cleanup(c);
        free(buf.data);
        return ESP_FAIL;
    }
    if (out_content_type) {
        *out_content_type = dup_http_header_value(c, "Content-Type");
        if (!*out_content_type) {
            char *hdr = NULL;
            /* strdup 失败时 esp_http_client_get_header 仍可能返回同一指针（非 NULL） */
            if (esp_http_client_get_header(c, "Content-Type", &hdr) == ESP_OK && hdr) {
                free(buf.data);
                esp_http_client_cleanup(c);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    esp_http_client_cleanup(c);
    *out_data = buf.data;
    *out_len = buf.len;
    return ESP_OK;
}

esp_err_t cloud_http_post_multipart_capture(const char *url, const char *auth_bearer,
                                            const char *content, size_t content_len,
                                            char **out_response)
{
    if (!url || !content || !out_response) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_response = NULL;

    static const char boundary[] = "----ESP32VoiceBoundary";
    const char *model = "whisper-1";
    char prefix[192];
    char suffix[128];
    int prefix_len = snprintf(prefix, sizeof(prefix),
                              "--%s\r\n"
                              "Content-Disposition: form-data; name=\"file\"; filename=\"rec.wav\"\r\n"
                              "Content-Type: audio/wav\r\n"
                              "\r\n",
                              boundary);
    int suffix_len = snprintf(suffix, sizeof(suffix),
                              "\r\n--%s\r\n"
                              "Content-Disposition: form-data; name=\"model\"\r\n"
                              "\r\n"
                              "%s"
                              "\r\n--%s--\r\n",
                              boundary, model, boundary);
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(prefix) ||
        suffix_len <= 0 || suffix_len >= (int)sizeof(suffix)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (content_len > (size_t)INT_MAX - (size_t)prefix_len - (size_t)suffix_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    char ctype[128];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);

    buf_t buf = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "Content-Type", ctype);
    if (auth_bearer && strlen(auth_bearer) > 0) {
        char hdr[512];
        snprintf(hdr, sizeof(hdr), "Bearer %s", auth_bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    esp_err_t err = esp_http_client_open(c, prefix_len + (int)content_len + suffix_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    err = http_client_write_all(c, prefix, (size_t)prefix_len);
    if (err == ESP_OK) {
        size_t off = 0;
        while (off < content_len) {
            size_t chunk = content_len - off;
            if (chunk > 4096) {
                chunk = 4096;
            }
            err = http_client_write_all(c, content + off, chunk);
            if (err != ESP_OK) {
                break;
            }
            off += chunk;
        }
    }
    if (err == ESP_OK) {
        err = http_client_write_all(c, suffix, (size_t)suffix_len);
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) {
        err = ESP_FAIL;
    }
    int code = esp_http_client_get_status_code(c);
    if (err == ESP_OK) {
        err = http_read_response_text(c, &buf);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        free(buf.data);
        return err;
    }
    if (code < 200 || code >= 300) {
        ESP_LOGE(TAG, "multipart status %d (response body suppressed)", code);
        free(buf.data);
        return ESP_FAIL;
    }
    *out_response = (char *)buf.data;
    return ESP_OK;
}
