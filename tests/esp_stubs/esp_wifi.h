#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
/* WiFi stubs: host 语法检查/链接用(签名对齐 IDF 5.x,实现为无害空操作)。 */
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
    struct {
        uint8_t authmode;
    } threshold;
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
#define WIFI_MODE_STA 2
#define WIFI_MODE_APSTA 3
#define WIFI_IF_AP 1
#define WIFI_IF_STA 2
#define WIFI_AUTH_WPA2_PSK 4
#define WIFI_AUTH_OPEN 0
#define WIFI_STORAGE_RAM 1

typedef struct {
    uint32_t min;
    uint32_t max;
} wifi_active_scan_time_t;
typedef struct {
    wifi_active_scan_time_t active;
    uint32_t passive;
} wifi_scan_time_t;
typedef struct {
    uint8_t *ssid;
    uint8_t *bssid;
    uint8_t channel;
    bool show_hidden;
    wifi_scan_time_t scan_time;
} wifi_scan_config_t;
typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[33];
    uint8_t primary;
    int8_t rssi;
    uint8_t authmode;
} wifi_ap_record_t;

static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block) {
    (void)config; (void)block; return ESP_OK;
}
static inline esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number) {
    (void)number; return ESP_OK;
}
static inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *records) {
    (void)number; (void)records; return ESP_OK;
}

static const char k_wifi_event_base[] = "WIFI_EVENT";
#define WIFI_EVENT k_wifi_event_base
#define WIFI_EVENT_STA_START 0
#define WIFI_EVENT_STA_DISCONNECTED 3

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
static inline esp_err_t esp_wifi_connect(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
