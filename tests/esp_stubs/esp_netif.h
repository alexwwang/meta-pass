#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_netif esp_netif_t;

static const char k_ip_event_base[] = "IP_EVENT";
#define IP_EVENT k_ip_event_base
#define IP_EVENT_STA_GOT_IP 0

static inline esp_err_t esp_netif_init(void) { return ESP_OK; }
static inline esp_netif_t *esp_netif_create_default_wifi_ap(void) { return NULL; }
static inline esp_netif_t *esp_netif_create_default_wifi_sta(void) { return NULL; }
static inline void esp_netif_destroy_default_wifi(esp_netif_t *netif) { (void)netif; }
