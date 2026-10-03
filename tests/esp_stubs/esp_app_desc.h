#pragma once
#include <stdint.h>

// Host test stub: mirrors the device's esp_app_desc_t fields that the
#include <string.h>
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

// Stub for host tests: returns placeholder version

// IDF 5.5.3 签名对齐:无参、返回指向只读描述的指针。
// (此前虚构的 out 参数版只骗过了 host 语法检查,真编译即失败。)
const esp_app_desc_t *esp_app_get_description(void);
