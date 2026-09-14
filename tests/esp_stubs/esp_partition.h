#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

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
