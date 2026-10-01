// tests/esp_stubs/esp_rom_crc.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
#pragma once

#include <stdint.h>
#include <stddef.h>

uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len);
