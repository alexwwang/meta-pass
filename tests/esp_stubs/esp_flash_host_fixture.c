// tests/esp_stubs/esp_flash_host_fixture.c —— meta_carve_flash.c host 行为测试
// (tests/test_meta_carve_flash.c)链接所需的默认芯片与分区缓存桩。
// 背景:f909fc8 / 207dc1c 给设备侧加了 esp_partition_register_external /
// esp_partition_unload_all / esp_partition_is_flash_region_writable /
// esp_flash_default_chip 引用,host 桩未同步 → --static 门在该测试处断编译。
// 本文件补齐最小 host 语义(签名全部对齐 IDF 5.5.3 真头,见各条注释)。
#include <stddef.h>
#include <stdbool.h>

#include "esp_flash.h"
#include "esp_partition.h"

// 默认主 flash chip(IDF spi_flash/include/esp_flash.h:372)。host 钩子表全
// NULL:start/end 永不被调用,region_protected 只被 meta_carve_flash.c 复制/
// 改写;测试提供的 esp_flash_read/write/erase_region 假件忽略 os_func,
// 与 f909fc8 之前的 host 行为一致。
static esp_flash_os_functions_t s_host_os_func;
static esp_flash_t s_host_chip = { &s_host_os_func };
esp_flash_t *esp_flash_default_chip = &s_host_chip;

// IDF esp_partition.h:481 —— 设备上 free 掉 SRAM 分区缓存链表。host 的
// find_first 走 meta_store_host_fixture 的 host_parts 注册表,并非 IDF 缓存;
// 清它会改变既有测试语义 → 空操作(= f909fc8 之前的 host 行为)。
void esp_partition_unload_all(void)
{
}

// IDF esp_partition/partition_target.c:205 的 host 镜像:仅当与某个 readonly
// 分区重叠时返回 false,数据源为 host_parts 注册表(stub esp_partition.h)。
bool esp_partition_is_flash_region_writable(size_t addr, size_t size)
{
    for (int i = 0; i < host_part_count; i++) {
        const esp_partition_t *p = &host_parts[i];
        if (p->readonly) {
            if (addr >= p->address && addr < p->address + p->size) {
                return false;
            }
            if (addr < p->address && addr + size > p->address) {
                return false;
            }
        }
    }
    return true;
}
