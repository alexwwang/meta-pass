#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

// Minimal stubs for host test compilation (mirrors esp_image_format.h from IDF)

#define ESP_IMAGE_MAX_SEGMENTS 16
#define ESP_IMAGE_HASH_LEN 32

typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint8_t spi_mode;
    uint8_t spi_speed_size;
    uint32_t entry_addr;
    uint8_t wp_pin;
    uint8_t spi_pads[3];
    uint16_t chip_id;
    uint8_t min_chip_rev;
    uint8_t max_chip_rev;
    uint8_t reserved;
    uint8_t hash_appended;
} esp_image_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} esp_image_segment_header_t;

typedef struct {
    uint32_t start_addr;
    esp_image_header_t image;
    esp_image_segment_header_t segments[ESP_IMAGE_MAX_SEGMENTS];
    uint32_t segment_data[ESP_IMAGE_MAX_SEGMENTS];
    uint32_t image_len;
    uint8_t image_digest[ESP_IMAGE_HASH_LEN];
    uint32_t secure_version;
    uint32_t mmu_page_size;
} esp_image_metadata_t;

typedef enum {
    ESP_IMAGE_VERIFY,
    ESP_IMAGE_VERIFY_SILENT,
} esp_image_load_mode_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

// Host test stub: parses segment table to compute image_len (mirrors IDF esp_image_verify).
// Returns ESP_FAIL if the image is not valid.
esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *part, esp_image_metadata_t *data);
