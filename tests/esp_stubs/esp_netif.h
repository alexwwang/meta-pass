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

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info) {
    (void)netif; (void)info; return ESP_OK;
}

// DNS info (r10.15c diagnostic logging) — mirrors esp_netif.h IDF 5.5.3:
// esp_netif_dns_info_t = { esp_ip_addr_t ip } with type/union; host tests only
// need the .u_addr.ip4 path used by the diagnostic log.
typedef struct { int type; union { esp_ip4_addr_t ip4; } u_addr; } esp_netif_dns_ip_addr_t;
typedef struct { esp_netif_dns_ip_addr_t ip; } esp_netif_dns_info_t;
#define ESP_NETIF_DNS_MAIN 0
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(a) (unsigned)((a)->addr & 0xFF), (unsigned)(((a)->addr >> 8) & 0xFF), (unsigned)(((a)->addr >> 16) & 0xFF), (unsigned)(((a)->addr >> 24) & 0xFF)
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *ifkey) {
    (void)ifkey; return NULL;
}
static inline esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *info) {
    (void)netif; (void)type; (void)info; return ESP_OK;
}
