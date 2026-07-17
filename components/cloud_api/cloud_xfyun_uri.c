/**
 * 讯飞 wss 鉴权 URI。启用讯飞 ASR/TTS 时使用真实鉴权；均未启用时提供桩函数供链接。
 */
#include "cloud_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "net_wifi.h"
#include "sdkconfig.h"

#if CONFIG_CLOUD_ASR_USE_XFYUN_WS || CONFIG_CLOUD_TTS_USE_XFYUN_WS

esp_err_t cloud_xfyun_build_wss_uri(char *uri, size_t uri_cap, const char *host_str, const char *path_str)
{
    if (!uri || uri_cap == 0 || !host_str || !path_str || path_str[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 讯飞鉴权要求本机时间和服务器相差 ≤ 5 分钟；若 NTP 还没同步则主动触发并等待，
     * 最多 20 秒；同步失败则明确报错，避免 403 后一头雾水 */
    /* 必须在任何可能 goto cleanup 之前初始化，否则跳过 malloc 时 free/return 未定义 */
    char *date = NULL;
    char *sign_in = NULL;
    char *sig_b64 = NULL;
    char *auth_origin = NULL;
    unsigned char *auth_b64_bin = NULL;
    char *enc_auth = NULL;
    char *enc_date = NULL;
    char *enc_host = NULL;
    esp_err_t r = ESP_FAIL;

    if (time(NULL) < 1704067200) {
        esp_err_t te = net_wifi_wait_ntp(20000);
        if (te != ESP_OK) {
            ESP_LOGE("cloud_xfyun", "NTP not synced after 20s; abort (避免 403)");
            goto cleanup;
        }
    }

    /* 全部在堆上分配，避免累积 2KB+ 栈占用；函数退出统一释放 */
    date = (char *)malloc(48);
    sign_in = (char *)malloc(256);
    sig_b64 = (char *)malloc(64);
    auth_origin = (char *)malloc(384);
    auth_b64_bin = (unsigned char *)malloc(512);
    enc_auth = (char *)malloc(768);
    enc_date = (char *)malloc(256);
    enc_host = (char *)malloc(128);
    if (!date || !sign_in || !sig_b64 || !auth_origin || !auth_b64_bin || !enc_auth || !enc_date || !enc_host) {
        r = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    time_t now = time(NULL);
    cloud_format_rfc1123_gmt(date, 48, now);

    snprintf(sign_in, 256, "host: %s\ndate: %s\nGET %s HTTP/1.1", host_str, date, path_str);

    if (cloud_hmac_sha256_b64_sig(CONFIG_XFYUN_API_SECRET, sign_in, sig_b64, 64) != 0) {
        goto cleanup;
    }

    snprintf(auth_origin, 384,
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             CONFIG_XFYUN_API_KEY, sig_b64);

    size_t auth_b64_len = 0;
    if (mbedtls_base64_encode(auth_b64_bin, 511, &auth_b64_len,
                              (const unsigned char *)auth_origin, strlen(auth_origin)) != 0) {
        goto cleanup;
    }
    auth_b64_bin[auth_b64_len] = '\0';

    cloud_url_encode_component((char *)auth_b64_bin, enc_auth, 768);
    cloud_url_encode_component(date, enc_date, 256);
    cloud_url_encode_component(host_str, enc_host, 128);

    snprintf(uri, uri_cap, "wss://%s%s?authorization=%s&date=%s&host=%s", host_str, path_str, enc_auth,
             enc_date, enc_host);
    r = ESP_OK;

cleanup:
    free(date);
    free(sign_in);
    free(sig_b64);
    free(auth_origin);
    free(auth_b64_bin);
    free(enc_auth);
    free(enc_date);
    free(enc_host);
    return r;
}

#else /* !CONFIG_CLOUD_ASR_USE_XFYUN_WS && !CONFIG_CLOUD_TTS_USE_XFYUN_WS */

esp_err_t cloud_xfyun_build_wss_uri(char *uri, size_t uri_cap, const char *host_str, const char *path_str)
{
    (void)uri;
    (void)uri_cap;
    (void)host_str;
    (void)path_str;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
