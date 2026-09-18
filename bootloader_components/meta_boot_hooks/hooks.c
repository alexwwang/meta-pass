/*
 * SPDX-License-Identifier: MIT
 *
 * meta_boot_hooks/hooks.c — meta-pass 开机策略的 bootloader 执行层。
 *
 * 机制:IDF 2nd-stage bootloader 在 bootloader_init()(flash 可用)之后、
 * 分区选择(bootloader_start.c:64)之前调用弱符号 bootloader_after_init()
 * (bootloader_start.c:39-42)。本项目下存在 bootloader_components/ 目录时,
 * 构建系统自动把该组件链接进 bootloader(链接靠 bootloader_hooks_include()
 * 符号强制拉入,见 IDF custom_bootloader 示例)。
 *
 * 职责(单次会话模型,策略逻辑见 main/meta_boot_policy.h):
 *   每次启动、任何应用运行之前,检查 otadata 两个 32 字节副本:
 *   凡 ota_state == VALID 的副本 → 擦除其扇区。
 *   效果:子固件写 VALID 也无法常驻 —— 下一次上电 bootloader 必然找不到
 *   候选(或只剩 PENDING 走 trial-run),默认回 factory 列表页。
 *   开机策略由 meta-pass 的 bootloader 单方面执行,与子固件行为无关。
 *
 * flash 访问:bootloader_flash_read/erase_sector(bootloader_flash_priv.h,
 * IDF 自身写 otadata 即用此 API,bootloader_utility.c:312-314)。
 * 加密 flash:bootloader_flash_read(allow_decrypt=false)读到的是密文原样,
 * 此时 state 判定不可靠 → 检测到 flash 加密启用则放弃干预(与 IDF write_
 * otadata 的 write_encrypted 对应;本设备未启用加密,防御性处理)。
 *
 * 日志:ESP_LOGI 在此阶段输出到 UART0(与 "boot:" 前缀日志同通道),
 * 便于真机串口核对策略是否生效。
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_flash_partitions.h"
#include "bootloader_flash_priv.h"

#include "meta_boot_policy.h"

/* 与组件名一致:强制链接器保留本文件符号(IDF hooks 机制约定)。 */
void bootloader_hooks_include(void)
{
}

static const char *TAG = "meta-boot";

/* otadata 扇区地址由分区表逐条扫描得出,不硬编码 —— 分区表布局将来若调整,
 * 策略自动跟随(与安装页"读回比对"同一自适应哲学)。 */
static bool find_otadata_pos(uint32_t *out_offset)
{
    for (uint32_t addr = ESP_PARTITION_TABLE_OFFSET;
         addr < ESP_PARTITION_TABLE_OFFSET + ESP_PARTITION_TABLE_MAX_LEN;
         addr += sizeof(esp_partition_info_t)) {
        esp_partition_info_t entry;
        if (bootloader_flash_read(addr, &entry, sizeof(entry), false) != ESP_OK) {
            return false;
        }
        if (entry.magic != ESP_PARTITION_MAGIC) {
            break; /* 条目区以全 0xFF(空 magic)终止 */
        }
        if (entry.type == PART_TYPE_DATA && entry.subtype == PART_SUBTYPE_DATA_OTA) {
            *out_offset = entry.pos.offset;
            return true;
        }
    }
    return false;
}

/* 读取并判定一个副本;需要擦除时执行"擦除 → 读回 → 复核"。
 * 返回 true 表示已执行擦除。 */
static bool enforce_single_session_on_copy(uint32_t ota_offset, uint32_t copy_index)
{
    const uint32_t sector = ota_offset / 4096u + meta_boot_policy_copy_sector(copy_index);
    meta_otadata_entry_t entry;

    if (bootloader_flash_read(sector * 4096u, &entry, sizeof(entry), false) != ESP_OK) {
        return false;
    }
    if (!meta_boot_policy_entry_must_erase(&entry)) {
        return false;
    }

    ESP_LOGI(TAG, "otadata copy %u in VALID state -> erasing (single-session policy)",
             (unsigned)copy_index);
    if (bootloader_flash_erase_sector(sector) != ESP_OK) {
        ESP_LOGE(TAG, "erase otadata copy %u failed", (unsigned)copy_index);
        return false;
    }
    /* 复核:擦除后该扇区应为全 0xFF,state 读回 0xFFFFFFFF(≠ VALID)。 */
    meta_otadata_entry_t check;
    if (bootloader_flash_read(sector * 4096u, &check, sizeof(check), false) != ESP_OK ||
        check.ota_state == META_OTA_IMG_VALID) {
        ESP_LOGE(TAG, "otadata copy %u still VALID after erase", (unsigned)copy_index);
    }
    return true;
}

void bootloader_after_init(void)
{
    /* 深睡眠唤醒:跳过一切 flash 写入(策略文件头的防御性例外)。 */
    if (esp_rom_get_reset_reason(0) == RESET_REASON_CORE_DEEP_SLEEP) {
        return;
    }

#if CONFIG_SECURE_FLASH_ENC_ENABLED
    /* 加密 flash 下 bootloader_flash_read(false) 读到密文,判定不可靠 →
     * 不干预(本设备未启用加密;启用时策略退化为不生效,而非误擦)。 */
    return;
#endif

    uint32_t ota_offset = 0;
    if (!find_otadata_pos(&ota_offset)) {
        return; /* 无 otadata 分区:策略无对象,直接放行 */
    }

    bool erased_any = false;
    for (uint32_t i = 0; i < 2; ++i) {
        erased_any |= enforce_single_session_on_copy(ota_offset, i);
    }
    if (erased_any) {
        ESP_LOGI(TAG, "single-session policy enforced; bootloader will default to factory/launcher");
    }
}

/* before-init 钩子保持默认(弱符号未定义时 bootloader 跳过调用);
 * 显式不定义,避免在 BSS/flash 初始化前引入任何风险。 */
