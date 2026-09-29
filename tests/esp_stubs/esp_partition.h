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

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *src, size_t size);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *dest, size_t size);

// BUG-21 host-test support: fixture-installed partition registry searched by
// esp_partition_find_first, mirroring the device's subtype lookup.
#define HOST_PART_MAX 8
extern esp_partition_t host_parts[HOST_PART_MAX];
extern int host_part_count;
const esp_partition_t *esp_partition_find_first(uint8_t type, uint8_t subtype, const char *label);
