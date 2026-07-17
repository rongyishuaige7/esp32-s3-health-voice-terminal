#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** RFC1123 GMT 时间串（讯飞 wss 鉴权 date 参数） */
void cloud_format_rfc1123_gmt(char *buf, size_t sz, time_t t);

/** HMAC-SHA256(sign_in) 再 Base64，用于讯飞鉴权 signature */
int cloud_hmac_sha256_b64_sig(const char *secret, const char *sign_in, char *sig_b64, size_t sig_b64_sz);

/** application/x-www-form-urlencoded 风格的百分号编码（用于 authorization/date/host） */
void cloud_url_encode_component(const char *in, char *out, size_t out_cap);

/** 分段 xSemaphoreTake + 喂 Task WDT，避免长阻塞触发 TWDT */
bool cloud_wait_sem_feed_wdt(SemaphoreHandle_t sem, uint32_t timeout_ms);

/**
 * 构建讯飞开放平台流式 API 的 wss://...?authorization=...&date=...&host=...
 * @param host_str 如 "iat-api.xfyun.cn" 或 "tts-api.xfyun.cn"
 * @param path_str 如 "/v2/iat" 或 "/v2/tts"（须含前导 /）
 */
esp_err_t cloud_xfyun_build_wss_uri(char *uri, size_t uri_cap, const char *host_str, const char *path_str);

esp_err_t cloud_http_post_json_capture(const char *url, const char *auth_bearer,
                                       const char *json_body, char **out_response);

esp_err_t cloud_http_post_multipart_capture(const char *url, const char *auth_bearer,
                                            const char *content, size_t content_len,
                                            char **out_response);

esp_err_t cloud_http_post_binary_response(const char *url, const char *auth_bearer,
                                          const char *json_body,
                                          uint8_t **out_data, size_t *out_len,
                                          char **out_content_type);

#ifdef __cplusplus
}
#endif
