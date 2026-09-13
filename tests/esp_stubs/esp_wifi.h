#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
/* WiFi stubs: only used by meta_net_start/stop which we don't call.
 * Defined to satisfy the linker without pulling in real ESP-IDF. */
typedef struct {
    char ssid[32];
    uint8_t ssid_len;
    char password[64];
    uint8_t channel;
    uint8_t authmode;
    uint8_t max_connection;
    uint16_t beacon_interval;
} wifi_ap_config_t;
typedef struct {
    char ssid[32];
    char password[64];
} wifi_sta_config_t;
typedef struct {
    union {
        wifi_ap_config_t ap;
        wifi_sta_config_t sta;
    };
} wifi_config_t;
typedef struct {
    int dummy;
} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){.dummy = 0})
#define WIFI_MODE_AP 1
#define WIFI_IF_AP 1
#define WIFI_AUTH_WPA2_PSK 4
#define WIFI_STORAGE_RAM 1
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *config) {
    (void)config; return ESP_OK;
}
static inline esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config) {
    (void)interface; (void)config; return ESP_OK;
}
static inline esp_err_t esp_wifi_start(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_stop(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_deinit(void) { return ESP_OK; }
