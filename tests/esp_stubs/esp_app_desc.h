#pragma once
#include <stdint.h>

// Host test stub: mirrors the device's esp_app_desc_t fields that the
// meta-store scan path consumes (esp_app_desc.h, IDF 5.5.3).
// Only the leading fields matter for host tests; offsets match the real
// struct up to project_name/version so the 64 B read contract holds.
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint8_t reserv1[2];
    char version[32];
    char project_name[32];
    uint8_t reserv2[16];
} esp_app_desc_t;
