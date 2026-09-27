// tests/esp_stubs/esp_netif_sntp.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool        smooth_sync;
    bool        server_from_dhcp;
    bool        wait_for_sync;
    bool        start;
    const char *servers[4];
    uint8_t     num_of_servers;
} esp_sntp_config_t;

// IDF 宏展开为带默认值的初始化器;桩侧提供同名函数声明即可语法检查。
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) \
    ((esp_sntp_config_t){ .wait_for_sync = true, .servers = { server }, .num_of_servers = 1 })

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
esp_err_t esp_netif_sntp_sync_wait(TickType_t tick_to_wait);
void esp_netif_sntp_deinit(void);

#ifdef __cplusplus
}
#endif
