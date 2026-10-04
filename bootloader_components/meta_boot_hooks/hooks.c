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
 * 职责(策略逻辑见 main/meta_boot_policy.h),按复位原因分两条路径:
 *
 *   1) 冷启动(上电/看门狗/崩溃/软件重启):
 *      检查 otadata 两个 32 字节副本,凡 ota_state == VALID 的副本 → 擦除其扇区。
 *      效果:子固件写 VALID 也无法常驻 —— 下一次上电 bootloader 必然找不到
 *      候选(或只剩 PENDING 走 trial-run),默认回 factory 列表页。
 *      开机策略由 meta-pass 单方面执行,与子固件行为无关。
 *
 *   2) 深睡眠唤醒:
 *      不擦除,而是把 PENDING_VERIFY 的副本续期为 VALID,使 bootloader 直接引导
 *      回该槽位 —— 子固件按自身空闲超时(如 60s)息屏后,按键唤醒即"续玩"。
 *      PENDING 只可能由 bootloader 在选择某个 OTA 槽时写入,应用侧无法伪造,故
 *      "读到 PENDING" 等价于"上次引导的是子固件且它尚未确认"。
 *      续期出的 VALID 会在下一次冷启动被路径 1 擦除,不产生跨上电常驻。
 *
 *      为何不用 CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP:该选项把"上次
 *      引导的分区"记在 RTC 快速内存顶部(rtc_retain_mem_t)。子固件各自的构建
 *      默认不含 CONFIG_BOOTLOADER_RESERVE_RTC_MEM,其链接脚本不为 bootloader
 *      预留该区域(RTC 定时器数据正好覆盖那条记录),子固件一运行记录即失效,
 *      快速引导静默回退常规路径 → 仍回 factory。该选项要求每个子固件配合,
 *      与"系统级策略不得委托子固件"(AGENTS.md)冲突。改写 otadata 无此依赖。
 *
 * flash 访问:bootloader_flash_read/erase_sector/write(bootloader_flash_priv.h,
 * IDF 自身写 otadata 即用此 API,bootloader_utility.c:310-320)。
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
#include "bootloader_common.h"   /* bootloader_common_ota_select_crc */

#include "meta_boot_policy.h"
#include "meta_carve_boot.h"   // dynslot 表裁决纯逻辑(§4.4,B5 权威)

/* 策略判定函数接收 meta_otadata_entry_t(与宿主测试共享的镜像结构),而 flash
 * 读写用 IDF 的 esp_ota_select_entry_t;两者必须逐字段同布局,故在此钉死
 * (字段偏移的等价断言在 tests/test_meta_boot_policy.c 里另有覆盖)。 */
_Static_assert(sizeof(meta_otadata_entry_t) == sizeof(esp_ota_select_entry_t),
               "otadata entry mirror must match esp_ota_select_entry_t");
_Static_assert(offsetof(meta_otadata_entry_t, ota_state) ==
               offsetof(esp_ota_select_entry_t, ota_state),
               "ota_state offset must match esp_ota_select_entry_t");

/* 与组件名一致:强制链接器保留本文件符号(IDF hooks 机制约定)。 */
void bootloader_hooks_include(void)
{
}

static const char *TAG = "meta-boot";

/* otadata 扇区地址与 OTA 槽数量由分区表逐条扫描得出,不硬编码 —— 分区表布局
 * 将来若调整,策略自动跟随(与安装页"读回比对"同一自适应哲学)。
 * 槽计数规则与 IDF 一致:subtype 高位为 OTA 标志、低位为槽号
 * (bootloader_utility.c:193-196);续期时用 slot = (ota_seq-1) % count 还原
 * 槽号,与 bootloader 的映射公式(bootloader_utility.c:365)同源。 */
static bool scan_partition_table(uint32_t *out_ota_offset, uint32_t *out_ota_count)
{
    bool found_ota_data = false;
    uint32_t ota_count = 0;

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
            *out_ota_offset = entry.pos.offset;
            found_ota_data = true;
        } else if (entry.type == PART_TYPE_APP &&
                   (entry.subtype & ~PART_SUBTYPE_OTA_MASK) == PART_SUBTYPE_OTA_FLAG) {
            ++ota_count;
        }
    }

    *out_ota_count = ota_count;
    return found_ota_data;
}

/* ── dynslot carve 表强制(设计 §4.4,边界 B5) ─────────────────────────
 *
 * 读 store 固定地址上的 A/B carve 记录(不依赖表 —— 表正是被校验对象),
 * 与 0x8000 live 表逐字节比对:
 *   合法记录 + 表一致        → 放行(冷启动单次会话策略照旧);
 *   合法记录 + 表不一致      → 用记录内嵌表重写 0x8000,清 otadata → factory;
 *   无合法记录               → live ∈ {内置安全表, legacy v1.x 表} 放行,
 *                               否则写回内置安全表并清 otadata(§4.7 store 死 /
 *                               子固件乱写)。
 * 修复先于任何 otadata 决策 —— 静态门 tests/test_dynslot_hook_gate.py 钉死顺序。
 *
 * 表写:单 4KB 扇区,擦 → 写 → 读回复核;中断残留由下一次开机的本函数再修复
 * (记录仍在 store → 幂等重写)。otadata 偏移从“新写入的表”扫描得出 —— 绝不
 * 用旧表(子固件表的 otadata 可能在别处,如 play 563 的 0x310000)。
 * 缓冲全部 static:bootloader 栈小。BSS 预算(dram_seg 实测溢出 8184B 的修复):
 * 旧版同时持有两条完整记录(s_best+s_tmp)+ 记录扇区 + live 表 + 读回复核
 * ≈ 17.9KB;现改为单一 4KB 工作缓冲(两扇区轮流读 → 胜者物化 → live 表 →
 * 读回复核,顺序阶段互不重叠)+ 单条记录 ≈ 7.9KB,并用 raw_info 先比 seq、
 * 只对胜者跑完整 decode(格式层零栈开销,decode 也不再上 3.9KB 栈)。
 */

/* 单一工作缓冲(4KB 记录扇区 ≥ 3KB 表视图,顺序复用)+ 仅持有胜者记录。 */
static uint8_t s_scratch[META_CARVE_REC_SIZE];
static meta_carve_rec_t s_best;

static bool meta_carve_boot_restore(const uint8_t table[META_PT_SIZE])
{
    if (table == NULL) {
        return false;
    }
    const uint32_t sector = ESP_PARTITION_TABLE_OFFSET / 4096u;
    if (bootloader_flash_erase_sector(sector) != ESP_OK) {
        ESP_LOGE(TAG, "carve: erase table sector failed");
        return false;
    }
    if (bootloader_flash_write(ESP_PARTITION_TABLE_OFFSET, table, META_PT_SIZE,
                               false) != ESP_OK) {
        ESP_LOGE(TAG, "carve: write table failed");
        return false;
    }
    /* 读回复核走共享缓冲:此刻 live 视图已消费完(decide 不会把 v.table 指回
     * 输入缓冲 —— 指向 s_best.table 或 meta_carve_boot 的 s_ref)。 */
    if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET, s_scratch, META_PT_SIZE,
                              false) != ESP_OK ||
        memcmp(s_scratch, table, META_PT_SIZE) != 0) {
        ESP_LOGE(TAG, "carve: table read-back mismatch");
        return false;
    }
    /* 新表(安全/记录/legacy 三种来源都必然声明 otadata)扫描取偏移后清两扇区
     * → 本次引导无候选 → factory。扫描失败按 B1 不可动契约回退 0x7FE000。 */
    uint32_t ota_offset = 0;
    uint32_t ota_count = 0;
    if (!scan_partition_table(&ota_offset, &ota_count) || ota_offset == 0) {
        ESP_LOGE(TAG, "carve: no otadata entry after restore; using fixed 0x7FE000");
        ota_offset = 0x7FE000u;
    }
    for (uint32_t copy = 0; copy < 2u; ++copy) {
        if (bootloader_flash_erase_sector(ota_offset / 4096u + copy) != ESP_OK) {
            ESP_LOGE(TAG, "carve: erase otadata copy %u failed", (unsigned)copy);
            return false;
        }
    }
    ESP_LOGW(TAG, "carve: table restored; otadata wiped -> factory boot");
    return true;
}

/* 读一个扇区进共享缓冲;失败 false。 */
static bool load_sector(uint32_t addr)
{
    return bootloader_flash_read(addr, s_scratch, META_CARVE_REC_SIZE, false)
           == ESP_OK;
}

/* 解码+结构校验一个扇区到 s_best(胜者物化;失败 s_best 内容未定义)。 */
static bool load_record(uint32_t addr)
{
    return load_sector(addr) &&
           meta_carve_rec_decode(s_scratch, &s_best) &&
           meta_carve_rec_validate(&s_best);
}

static void enforce_carve_table(void)
{
    bool have = false;

    /* A/B 记录按固定裸地址读(store 分区尚可能不在表里,如 legacy 首启)。
     * 轻校验(raw_info:magic/version/seq/CRC,零物化)先比 seq,只对胜者
     * 跑完整 decode —— 两条记录不再同时驻留 BSS。胜者结构校验不过(损坏
     * 窗口)时退回另一扇区全量校验一次,与旧版“逐条验、新者胜”语义一致。 */
    int win = -1;
    uint32_t win_seq = 0;
    for (uint32_t i = 0; i < 2u; ++i) {
        const uint32_t addr =
            META_STORE_OFFSET + i * META_STORE_SECTOR_SIZE;
        if (!load_sector(addr)) {
            continue;
        }
        uint32_t seq = 0;
        if (!meta_carve_rec_raw_info(s_scratch, &seq)) {
            continue;   /* 损坏/撕裂/擦除态 → 当作无记录(§4.7) */
        }
        if (win < 0 || (int32_t)(seq - win_seq) > 0) {
            win = (int)i;   /* 新者胜(回绕比较) */
            win_seq = seq;
        }
    }
    if (win >= 0) {
        const uint32_t base = META_STORE_OFFSET;
        have = load_record(base + (uint32_t)win * META_STORE_SECTOR_SIZE);
        if (!have) {
            have = load_record(base + (1u - (uint32_t)win) * META_STORE_SECTOR_SIZE);
        }
    }

    if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET, s_scratch,
                              META_PT_SIZE, false) != ESP_OK) {
        ESP_LOGE(TAG, "carve: cannot read live table; skipping enforcement");
        return;
    }
    const meta_boot_table_verdict_t v =
        meta_carve_boot_decide(s_scratch, have ? &s_best : NULL);
    if (v.action == META_BOOT_TABLE_PROCEED) {
        ESP_LOGI(TAG, "carve: %s", v.reason);
        return;
    }
    ESP_LOGW(TAG, "carve repair: %s", v.reason);
    meta_carve_boot_restore(v.table);
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

/* 深睡唤醒:把"上次引导的子固件"续期为 VALID,使 bootloader 直接引导回该槽。
 *
 * 只改写 state 字段:ota_seq 与 crc 原样保留(crc 仅覆盖 ota_seq,续期不改它,
 * 故 CRC 依然有效 —— 这是把 VALID 写回同一副本而非新建条目的前提)。
 * 用 IDF 的 bootloader_common_ota_select_crc() 复核 CRC,避免自行实现漂移。
 *
 * 为什么先擦除整个扇区再写:flash 只能把 1 写成 0,无法把已写字节改回 1;
 * PENDING(0x1) → VALID(0x2) 需要把 bit1 从 0 置 1,必须擦后重写。
 * 擦除会丢掉同扇区其余内容 —— 但该扇区只承载这一个 32 字节副本(IDF 布局),
 * 且我们紧接着把完整副本写回,无信息损失。
 *
 * 返回 true 表示已执行续期。 */
static bool resume_running_slot_on_copy(uint32_t ota_offset, uint32_t ota_count,
                                        uint32_t copy_index)
{
    const uint32_t sector = ota_offset / 4096u + meta_boot_policy_copy_sector(copy_index);
    const uint32_t addr = sector * 4096u;
    esp_ota_select_entry_t entry;

    if (bootloader_flash_read(addr, &entry, sizeof(entry), false) != ESP_OK) {
        return false;
    }
    /* 策略判定用共享的 32 字节镜像结构,字段布局一致(宿主测试钉死)。 */
    if (!meta_boot_policy_entry_must_resume((const meta_otadata_entry_t *)&entry)) {
        return false;
    }
    /* CRC 复核:坏副本交给 bootloader 按原逻辑判无效并回退 factory,不强行续期。 */
    if (entry.crc != bootloader_common_ota_select_crc(&entry)) {
        ESP_LOGW(TAG, "otadata copy %u looks PENDING but CRC is bad -> not resuming",
                 (unsigned)copy_index);
        return false;
    }

    const uint32_t slot = (entry.ota_seq - 1u) % ota_count;
    ESP_LOGI(TAG, "deep-sleep wake: resuming ota_%u (otadata copy %u PENDING -> VALID)",
             (unsigned)slot, (unsigned)copy_index);

    entry.ota_state = ESP_OTA_IMG_VALID;
    if (bootloader_flash_erase_sector(sector) != ESP_OK) {
        ESP_LOGE(TAG, "erase otadata copy %u failed", (unsigned)copy_index);
        return false;
    }
    if (bootloader_flash_write(addr, &entry, sizeof(entry), false) != ESP_OK) {
        ESP_LOGE(TAG, "write otadata copy %u failed", (unsigned)copy_index);
        return false;
    }
    /* 复核:state 应读回 VALID。 */
    esp_ota_select_entry_t check;
    if (bootloader_flash_read(addr, &check, sizeof(check), false) != ESP_OK ||
        check.ota_state != ESP_OTA_IMG_VALID) {
        ESP_LOGE(TAG, "otadata copy %u not VALID after resume write", (unsigned)copy_index);
    }
    return true;
}

void bootloader_after_init(void)
{
#if CONFIG_SECURE_FLASH_ENC_ENABLED
    /* 加密 flash 下 bootloader_flash_read(false) 读到密文,判定不可靠 →
     * 不干预(本设备未启用加密;启用时策略退化为不生效,而非误写)。 */
    return;
#endif

    /* 第一优先级:dynslot carve 表校验/修复(§4.4)——必须先于 otadata 决策,
     * 否则被篡改的表可能在修复前就左右本次引导(B5)。 */
    enforce_carve_table();

    uint32_t ota_offset = 0;
    uint32_t ota_count = 0;
    if (!scan_partition_table(&ota_offset, &ota_count)) {
        return; /* 无 otadata 分区:策略无对象,直接放行 */
    }
    if (ota_count == 0) {
        return; /* 无 OTA 槽:续期无对象(冷启动的擦除仍无妨,此处统一放行) */
    }

    const bool deep_sleep_wake =
        (esp_rom_get_reset_reason(0) == RESET_REASON_CORE_DEEP_SLEEP);

    bool changed = false;
    for (uint32_t i = 0; i < 2; ++i) {
        if (deep_sleep_wake) {
            /* 唤醒路径:续期正在运行的子固件,不擦 VALID(那是冷启动的职责)。 */
            changed |= resume_running_slot_on_copy(ota_offset, ota_count, i);
        } else {
            changed |= enforce_single_session_on_copy(ota_offset, i);
        }
    }

    if (deep_sleep_wake) {
        if (changed) {
            ESP_LOGI(TAG, "deep-sleep wake handled; bootloader will resume the child slot");
        } else {
            ESP_LOGI(TAG, "deep-sleep wake: no running child to resume; default boot applies");
        }
    } else if (changed) {
        ESP_LOGI(TAG, "single-session policy enforced; bootloader will default to factory/launcher");
    }
}

/* before-init 钩子保持默认(弱符号未定义时 bootloader 跳过调用);
 * 显式不定义,避免在 BSS/flash 初始化前引入任何风险。 */
