// tests/esp_stubs/esp_flash_partitions.h —— host 编译桩(IDF 5.5.3
// bootloader_support/include/esp_flash_partitions.h 对齐)。
// meta_carve_flash.c(207dc1c)只用到分区表偏移/长度常量与只读区检查;
// esp_partition_is_flash_region_writable 的 host 实现镜像 IDF
// esp_partition/partition_target.c:205(遍历 readonly 分区),见
// esp_flash_host_fixture.c。桩不链接真 IDF。
#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// IDF:58 —— CONFIG_PARTITION_TABLE_OFFSET(ESP32-C3 默认 0x8000;与本仓库
// launcher-upgrade.js PARTITION_TABLE_OFFSET 及设备布局常量同值)。
#ifndef ESP_PARTITION_TABLE_OFFSET
#define ESP_PARTITION_TABLE_OFFSET 0x8000
#endif
// IDF:63 —— 字面量 0xC00(分区表数据最大长度)。
#define ESP_PARTITION_TABLE_MAX_LEN 0xC00

// IDF:121 —— 区域不与任何 readonly 分区重叠时返回 true。
bool esp_partition_is_flash_region_writable(size_t addr, size_t size);

#ifdef __cplusplus
}
#endif
