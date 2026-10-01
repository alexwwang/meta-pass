// tests/esp_stubs/esp_flash.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
// 只提供 meta_store_net.c 裸 flash 凭证备份用到的 API;不链接,仅 -fsyntax-only。
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_flash_read(void *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_write(void *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_erase_region(void *chip, uint32_t start_address, uint32_t size);

#ifdef __cplusplus
}
#endif
