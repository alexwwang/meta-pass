/*
 * SPDX-License-Identifier: MIT
 *
 * meta_boot_policy.h — 开机策略纯逻辑(单次会话模型的规则引擎)。
 *
 * 设计原则(2026-09-18,用户确认):设备断电重启后是否回到 meta-pass 列表页,
 * 必须由 meta-pass 单方面决定,与子固件的行为(是否调用 mark_valid 等)完全无关。
 * 唯一能在任何应用运行之前做决定的层是 2nd-stage bootloader,因此该策略由
 * bootloader hook(bootloader_components/meta_boot_hooks/hooks.c)执行。
 *
 * ── 背景:IDF otadata 语义(全部从 IDF v5.5.3 源码核实)────────────────
 *   otadata 分区 = 2 个 32 字节副本,各占 1 扇区(副本 0 = 扇区 0,副本 1 = 扇区 1):
 *     { u32 ota_seq; u8 seq_label[20]; u32 ota_state; u32 crc; }
 *   副本被 bootloader 视为"可引导候选"的条件(bootloader_common_loader.c:79
 *   ota_select_valid):ota_seq != 0xFFFFFFFF && ota_state ∉ {INVALID, ABORTED}
 *   && crc 匹配(crc 只覆盖 ota_seq 字段)。
 *   两副本均无候选 → "Defaulting to factory"(bootloader_utility.c:412-415)。
 *   PENDING_VERIFY → bootloader 在选择前自动标 ABORTED(同文件 :405-410),
 *   随后自动落到剩余候选或 factory —— 这就是 trial-run 回滚。
 *
 * ── 单次会话不变量 ────────────────────────────────────────────────
 *   "otadata 副本的 ota_state 字段不得为 VALID(0x2)。"
 *   VALID 是唯一能让镜像跨上电常驻的状态:候选副本的 seq 映射到某个 OTA 槽,
 *   每次上电 bootloader 直接引导该槽,factory 列表页永不运行(即"锁死"根因)。
 *   因此凡读到 state==VALID 的副本,一律擦除其所在扇区 —— 无论写入者是谁
 *   (旧版 hook 的 cancel_rollback、任何第三方子固件),从机制上消灭"常驻"。
 *
 *   为什么不检查 CRC / ota_seq(保守性论证):
 *   - 真正会引发常驻的形态必然是"候选 && VALID"(CRC 合法是候选前提);
 *   - state==VALID 但 CRC 坏/seq 空的条目 bootloader 本就不引导,擦除只是
 *     清垃圾,零副作用;
 *   - 其余 state 取值(NEW/PENDING/INVALID/ABORTED/UNDEFINED=0xFFFFFFFF)
 *     都 != 0x2,擦除判定零误伤;擦除态副本 state 读回 0xFFFFFFFF,天然免擦。
 *   - PENDING 一律不碰:保住 IDF 的 trial-run 回滚与崩溃自恢复流程。
 *
 *   复位原因例外(hooks.c 负责):深睡眠唤醒跳过全部检查与写入 —— 唤醒路径
 *   必须零 flash 操作(设备当前不使用深睡眠,此为未来防御)。
 *
 * 本头文件只含纯判定逻辑(给定 32 字节副本 → 是否擦除),供 bootloader hook
 * 与宿主测试共享;flash 操作细节在 hooks.c。
 */

#ifndef META_BOOT_POLICY_H
#define META_BOOT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* otadata 副本在 flash 上的布局(esp_flash_partitions.h esp_ota_select_entry_t
 * 的镜像定义;32 字节,小端原样)。 */
#define META_OTADATA_ENTRY_SIZE 32u
#define META_OTADATA_SEQ_EMPTY  0xFFFFFFFFu

typedef struct {
    uint32_t ota_seq;
    uint8_t seq_label[20];
    uint32_t ota_state;
    uint32_t crc;
} meta_otadata_entry_t;

/* esp_ota_img_states_t 取值(esp_flash_partitions.h:66-76),小端 u32。 */
#define META_OTA_IMG_NEW            0x0u
#define META_OTA_IMG_PENDING_VERIFY 0x1u
#define META_OTA_IMG_VALID          0x2u
#define META_OTA_IMG_INVALID        0x3u
#define META_OTA_IMG_ABORTED        0x4u
#define META_OTA_IMG_UNDEFINED      0xFFFFFFFFu

/* 副本 i(0/1)所在的 otadata 分区内扇区号(每副本恰好 1 个 4KB 扇区)。 */
static inline uint32_t meta_boot_policy_copy_sector(uint32_t copy_index)
{
    return copy_index; /* 副本 0 → 分区内扇区 0,副本 1 → 扇区 1 */
}

/* ── 策略核心判定:该副本是否必须被擦除 ───────────────────────────
 * 规则:ota_state == VALID → 必擦(见文件头不变量与保守性论证)。
 * 调用方保证 e 是从 flash 副本起始处原样读出的 32 字节。 */
static inline bool meta_boot_policy_entry_must_erase(const meta_otadata_entry_t *e)
{
    if (e == NULL) {
        return false;
    }
    return e->ota_state == META_OTA_IMG_VALID;
}

#endif /* META_BOOT_POLICY_H */
