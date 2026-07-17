#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 NVS、事件循环、默认 netif、STA 模式并连接 CONFIG_WIFI_* */
esp_err_t net_wifi_init_and_connect(void);

/** 是否已获取 IP */
bool net_wifi_is_connected(void);

/** 阻塞等待 NTP 完成同步（已同步立即返回）。必要时内部会重新触发 SNTP 轮询。
 *  timeout_ms：总超时；返回 ESP_OK 表示 time(NULL) 已是 >= 2024 的真实时间。 */
esp_err_t net_wifi_wait_ntp(int timeout_ms);

#ifdef __cplusplus
}
#endif
