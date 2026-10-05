#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Partition type/subtype values mirror esp_partition.h (IDF 5.5.3).
typedef uint8_t esp_partition_type_t;
typedef uint8_t esp_partition_subtype_t;
#define ESP_PARTITION_TYPE_APP 0x00
#define ESP_PARTITION_TYPE_DATA 0x01
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0x00
#define ESP_PARTITION_SUBTYPE_DATA_NVS 0x02
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_MIN ESP_PARTITION_SUBTYPE_APP_OTA_0
#define ESP_PARTITION_SUBTYPE_APP_OTA(n) ((esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + (n)))
#define ESP_PARTITION_SUBTYPE_APP_FACTORY 0x00

typedef struct {
    uint8_t type;
    uint8_t subtype;
    uint32_t address;
    uint32_t size;
    uint32_t erase_size;
    char label[17];
    bool encrypted;
    bool readonly;
} esp_partition_t;

// IDF 5.5.3 esp_partition.h:462 —— 外部 flash 芯片分区注册(carve 物化后把新槽
// 注册进 esp_partition 缓存,meta_store_install.c 使用;esp_flash_t 前置声明见
// IDF spi_flash/include/esp_flash.h:21)。
typedef struct esp_flash_t esp_flash_t;
esp_err_t esp_partition_register_external(esp_flash_t *flash_chip, size_t offset, size_t size,
                                          const char *label, esp_partition_type_t type,
                                          esp_partition_subtype_t subtype,
                                          const esp_partition_t **out_partition);

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *src, size_t size);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *dest, size_t size);

// BUG-21 host-test support: fixture-installed partition registry searched by
// esp_partition_find_first, mirroring the device's subtype lookup.
#define HOST_PART_MAX 8
extern esp_partition_t host_parts[HOST_PART_MAX];
extern int host_part_count;
const esp_partition_t *esp_partition_find_first(uint8_t type, uint8_t subtype, const char *label);

// IDF 5.5.3 esp_partition.h:481 —— 失效 SRAM 分区缓存(f909fc8 物化后调用,
// 防旧表缓存把镜像写进别人的槽)。host 无该缓存,实现在 esp_flash_host_fixture.c(no-op)。
void esp_partition_unload_all(void);
