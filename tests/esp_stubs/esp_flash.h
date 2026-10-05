// tests/esp_stubs/esp_flash.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
// 只提供 meta_store_net.c 裸 flash 凭证备份用到的 API;不链接,仅 -fsyntax-only。
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// IDF 5.5.3:spi_flash/include/esp_flash.h:21(类型)、:372(默认芯片指针)。
// 与 esp_partition.h 桩中的前置声明同型重复 —— C11 允许同型 typedef 重定义。
typedef struct esp_flash_t esp_flash_t;
extern esp_flash_t *esp_flash_default_chip;   // 定义在 esp_flash_host_fixture.c

// IDF esp_flash_os_functions_t 的 host 子集:meta_carve_flash.c(207dc1c)只
// 复制整表并改写 region_protected(绕过悬空 esp_ota 缓存);其余成员设备专用,
// host 代码不引用,故不搬。
typedef struct {
    esp_err_t (*start)(void *arg);
    esp_err_t (*end)(void *arg);
    /** Called before any erase/write operations to check whether the region is limited by the OS */
    esp_err_t (*region_protected)(void *arg, size_t start_addr, size_t size);
} esp_flash_os_functions_t;

// host 最小布局:meta_carve_flash.c 仅访问 chip->os_func(grep 全文证实),
// 硬件字段(host/chip_drv/read_mode/…)设备专用,host 永不解引用 → 不搬。
struct esp_flash_t {
    const esp_flash_os_functions_t *os_func;
};

esp_err_t esp_flash_read(void *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_write(void *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_erase_region(void *chip, uint32_t start_address, uint32_t size);

#ifdef __cplusplus
}
#endif
