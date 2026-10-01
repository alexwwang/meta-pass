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
 *   深睡眠唤醒(hooks.c 负责):不执行上面的 VALID 擦除策略(那是冷启动的职责),
 *   而是把"子固件正在运行"的 PENDING_VERIFY 副本续期为 VALID,使 bootloader 直接
 *   引导回该槽位 —— 子固件按自身空闲超时息屏后,按键唤醒即"续玩"。判定见
 *   meta_boot_policy_entry_must_resume()。
 *
 *   为什么由启动器侧改写 otadata,而不用 IDF 的快速引导:
 *   CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP 把"上次引导的分区"记在 RTC
 *   快速内存顶部(rtc_retain_mem_t)。但子固件各自的构建默认不含
 *   CONFIG_BOOTLOADER_RESERVE_RTC_MEM,其链接脚本不为 bootloader 预留该区域
 *   (RTC 定时器数据正好覆盖那条记录),子固件一运行记录即失效 → 快速引导静默
 *   回退常规路径 → 仍回 factory。该选项因此要求每个子固件配合,与"系统级策略
 *   不得委托子固件"的原则冲突。改写 otadata 不依赖任何子固件配置。
 *
 *   安全性:非深睡复位(断电/看门狗/崩溃/软件重启)一律走上面的 VALID 擦除 →
 *   仍回 factory 列表页,单次会话模型不变;唤醒路径只把"已在运行的子固件"续期,
 *   不引入新的跨上电常驻可能(续期出的 VALID 会在下一次冷启动被擦除)。
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

/* ── 深睡唤醒判定:该副本是否应被续期为 VALID(继续引导该槽位) ─────
 * 规则:ota_state == PENDING_VERIFY 且 ota_seq 非空。
 *
 * 为什么 PENDING 恰好等价于"正在运行的子固件":PENDING_VERIFY 只可能由
 * bootloader 在选择某个 OTA 槽时写入(NEW→PENDING,见文件头 otadata 语义),
 * 应用侧无法产生它。故深睡唤醒时读到 PENDING,即"本机上次引导的是某个子固件、
 * 且它尚未确认自己"—— 要续的正是这个会话。
 *
 * 为什么不检查 CRC:CRC 仅覆盖 ota_seq(4 字节)。若副本恰好损坏,bootloader 的
 * ota_select_valid 会判其无效并照旧回退 factory,与不续期时的结果一致,零额外
 * 风险;反之在此重复实现 CRC 只会引入与 IDF 实现漂移的可能。
 *
 * 与 must_erase 的关系:两者互斥(VALID vs PENDING),分别服务于"冷启动收编"与
 * "深睡唤醒续期"两条路径;续期出的 VALID 会在下一次冷启动被 must_erase 擦除,
 * 故不产生跨上电常驻。 */
static inline bool meta_boot_policy_entry_must_resume(const meta_otadata_entry_t *e)
{
    if (e == NULL) {
        return false;
    }
    return e->ota_state == META_OTA_IMG_PENDING_VERIFY &&
           e->ota_seq != META_OTADATA_SEQ_EMPTY;
}

#endif /* META_BOOT_POLICY_H */
