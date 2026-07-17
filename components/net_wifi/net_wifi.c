#include "net_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "net_wifi";

static EventGroupHandle_t s_wifi_ev;
#define WIFI_CONNECTED_BIT BIT0
static bool s_sntp_started;
static bool s_netif_ready;
static bool s_event_loop_ready;
static bool s_wifi_driver_ready;
static bool s_wifi_handlers_registered;
static bool s_wifi_started;
static esp_netif_t *s_wifi_sta;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
    if (s_wifi_ev) {
        xEventGroupSetBits(s_wifi_ev, WIFI_CONNECTED_BIT);
    }
}

static void on_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
    uint8_t reason = ev ? ev->reason : 0;

    ESP_LOGW(TAG, "wifi disconnect, reason=%u", (unsigned)reason);

    if (s_wifi_ev) {
        xEventGroupClearBits(s_wifi_ev, WIFI_CONNECTED_BIT);
    }

    /* 认证失败 / 找不到 AP 等：避免无意义狂刷重连，交由上层 net_wifi_init_and_connect 重试 */
    if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_AUTH_EXPIRE ||
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason == WIFI_REASON_NO_AP_FOUND) {
        ESP_LOGW(TAG, "skip auto-reconnect (check SSID/password or router)");
        return;
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "retry connect failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t ensure_wifi_stack(void)
{
    if (!s_wifi_ev) {
        s_wifi_ev = xEventGroupCreate();
        if (!s_wifi_ev) {
            ESP_LOGE(TAG, "create wifi event group failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_netif_ready) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
        s_netif_ready = true;
    }

    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_event_loop_ready = true;
    }

    if (!s_wifi_sta) {
        s_wifi_sta = esp_netif_create_default_wifi_sta();
        if (!s_wifi_sta) {
            ESP_LOGE(TAG, "create default wifi sta failed");
            return ESP_FAIL;
        }
    }

    if (!s_wifi_driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
        s_wifi_driver_ready = true;
    }

    if (!s_wifi_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL),
                            TAG, "register got ip");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_disconnect, NULL),
                            TAG, "register disconnect");
        s_wifi_handlers_registered = true;
    }

    return ESP_OK;
}

esp_err_t net_wifi_init_and_connect(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    ESP_RETURN_ON_ERROR(ensure_wifi_stack(), TAG, "wifi stack");

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi config");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_wifi_started = true;
    }

    if (!net_wifi_is_connected()) {
        xEventGroupClearBits(s_wifi_ev, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "connecting to configured Wi-Fi access point ...");
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect");

        const TickType_t conn_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
        for (;;) {
            esp_task_wdt_reset();
            EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(500));
            if (bits & WIFI_CONNECTED_BIT) {
                break;
            }
            if (xTaskGetTickCount() >= conn_deadline) {
                ESP_LOGE(TAG, "connect timeout");
                return ESP_ERR_TIMEOUT;
            }
        }
    }

    if (!s_sntp_started) {
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org"));
#else
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
#endif
        if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
            s_sntp_started = true;
            /* 分片等待并喂狗：避免默认 5s Task WDT 在 esp_netif_sntp_sync_wait 内触发 */
            esp_err_t te = ESP_ERR_TIMEOUT;
            for (int i = 0; i < 150; i++) {
                esp_task_wdt_reset();
                te = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100));
                if (te == ESP_OK) {
                    break;
                }
            }
            if (te != ESP_OK) {
                ESP_LOGW(TAG, "NTP sync failed (讯飞等鉴权可能失败): %s", esp_err_to_name(te));
            } else {
                ESP_LOGI(TAG, "NTP time synced");
            }
        }
    }

    /* 降低 DTIM 省电导致的额外时延，利于语音/流式 API 时延稳定 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

bool net_wifi_is_connected(void)
{
    if (!s_wifi_ev) {
        return false;
    }
    return xEventGroupGetBits(s_wifi_ev) & WIFI_CONNECTED_BIT;
}

#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t net_wifi_wait_ntp(int timeout_ms)
{
    if (time(NULL) >= 1704067200) {
        return ESP_OK;
    }
    /* 若 SNTP 未启动（net_wifi_init_and_connect 过早返回时可能出现），重新启动 */
    if (!s_sntp_started) {
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org"));
#else
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
#endif
        if (esp_netif_sntp_init(&cfg) == ESP_OK) {
            s_sntp_started = true;
        }
    }
    /* 主动踢一下，重新向服务器发请求 */
    esp_netif_sntp_start();

    const TickType_t t0 = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while ((xTaskGetTickCount() - t0) < limit) {
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(200)) == ESP_OK) {
            break;
        }
        if (time(NULL) >= 1704067200) {
            break;
        }
    }
    return time(NULL) >= 1704067200 ? ESP_OK : ESP_ERR_TIMEOUT;
}
