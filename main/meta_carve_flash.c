// main/meta_carve_flash.c —— 见 meta_carve_flash.h。
//
// 断电安全顺序(design §4.7):
//   - 迁移/提交:先写 A/B 记录(记录扇区内 CRC 收尾),后物化 0x8000 表。
//     任一步撕裂:下一开机 hook 用仍在的旧/新记录修复或回退,永不半引导。
//   - 记录写:擦目标扇区 → 写 [0, CRC 末尾) → 读回重解码;撕裂落在未写的 CRC 上
//     → 解码即拒 → A/B 另一扇区胜出。
//   - Wi-Fi 凭据迁移(L6)先于记录写:原裸备份 0x35A000 正是 store 扇区0。
//
// 缓冲全部文件级 static:app_main 主任务栈仅 3584B(y2lin 文档实测过饿死),
// 记录 3.8KB、扇区 4KB、表 3KB 都不可上栈。同一时刻只有一个活动缓冲
// (单线程启动器),s_io 被扇区读/写/凭据搬移顺序复用。
#include "meta_carve_flash.h"

#include <string.h>

#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_flash_partitions.h"   // ESP_PARTITION_TABLE_* / esp_partition_is_flash_region_writable
#include "esp_log.h"

static const char *TAG = "meta_carve";

static bool s_reboot_pending;       // 运行时装过 carved 表 → 退出商店页时复位清账
static meta_carve_t s_carve;        // 规范 carve
static uint32_t     s_seq;          // 最新记录 seq
static bool         s_have_record;  // 规范 carve 已 committed
static bool         s_active;       // ensure 已执行
static meta_carve_t s_in;           // commit 入参快照(防调用方传 &s_best.carve
                                    // 被 load_best 覆写;见 commit 注释)
static meta_carve_t s_work;         // M5 状态翻转/回收的副本(单线程,与 s_in 互斥复用)

// 共享缓冲(单线程顺序复用,互不同时存活):
static uint8_t s_live[META_PT_SIZE];       // live 表视图 / 表写读回
static uint8_t s_ref[META_PT_SIZE];        // 安全/legacy 参考 + 提交读回
static uint8_t s_io[META_CARVE_REC_SIZE];  // 记录扇区读写 + 凭据扇区搬移 + 表写读回
static meta_carve_rec_t s_best;            // 最新合法记录
static meta_carve_rec_t s_tmp;             // A/B 比较暂存

// M5: 升级数据迁移 —— 在池内拷贝数据。
// src/dst 必须同属一个 pool segment且对齐合法;失败 → 错误码。
esp_err_t meta_carve_flash_data_copy(uint32_t src_offset, uint32_t size,
                                     uint32_t dst_offset)
{
    if (!size) return ESP_ERR_INVALID_SIZE;
    if (size % META_CARVE_SIZE_GRANULE != 0) return ESP_ERR_INVALID_SIZE;  // 擦除粒度
    const meta_pool_desc_t *p = meta_carve_pool();
    // 验证 src/dst 都在池内且对齐
    { const uint32_t check_offs[] = {src_offset, dst_offset};
    for (size_t ci = 0; ci < 2; ci++) {
        const uint32_t off = check_offs[ci];
        bool in_pool = false;
        for (int si = 0; si < 2; si++) {
            if (off >= p->seg[si].start && off + size <= p->seg[si].end) {
                in_pool = true;
                break;
            }
        }
        if (!in_pool) return ESP_ERR_INVALID_ARG;
        if (off % 0x10000u != 0) return ESP_ERR_INVALID_ARG;  // 64KB 对齐
        }
    }
    if (src_offset == dst_offset) return ESP_OK;  // 同址无操作
    // 源/目重叠 → 拒(原地拷贝方向歧义;升级路径落在新分配区,不应重叠)。
    if (src_offset < dst_offset + size && dst_offset < src_offset + size) {
        return ESP_ERR_INVALID_ARG;
    }

    // 目标区先擦除:NOR 只能 1→0,向残留旧数据直接写会得到 AND 结果(静默
    // 损坏)。size/offset 已按 4KB 粒度/64KB 对齐,可整段擦。
    if (esp_flash_erase_region(NULL, dst_offset, size) != ESP_OK) {
        ESP_LOGE(TAG, "data_copy erase failed @0x%08lx", (unsigned long)dst_offset);
        return ESP_FAIL;
    }
    // 分段拷贝(源/目可能在同一或不同 sector)
    uint32_t remaining = size;
    uint32_t src = src_offset;
    uint32_t dst = dst_offset;
    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > META_CARVE_REC_SIZE) chunk = META_CARVE_REC_SIZE;
        if (esp_flash_read(NULL, s_io, src, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "data_copy read failed @0x%08lx", (unsigned long)src);
            return ESP_FAIL;
        }
        if (esp_flash_write(NULL, s_io, dst, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "data_copy write failed @0x%08lx", (unsigned long)dst);
            return ESP_FAIL;
        }
        src += chunk;
        dst += chunk;
        remaining -= chunk;
    }
    return ESP_OK;
}

void meta_carve_flash_test_reset(void)
{
    memset(&s_carve, 0, sizeof(s_carve));
    s_seq = 0;
    s_have_record = false;
    s_active = false;
}

const meta_carve_t *meta_carve_flash_carve(void)
{
    return &s_carve;
}

bool meta_carve_flash_has_record(void)
{
    return s_have_record;
}

// ---- flash 写保护钩子(悬空缓存规避) --------------------------------------
//
// IDF 默认 main_flash_region_protected 经 esp_partition_main_flash_region_safe
// 调 esp_ota_get_running_partition(),而后者把"指向 esp_partition SRAM 缓存
// 项"的指针做了进程级静态缓存(esp_ota_ops.c curr_partition)。物化路径的
// esp_partition_unload_all() 会 free 整个缓存链表 —— 该静态指针悬空。堆块
// 被复用/填毒(0xa5a5a5a5)后,任何一次 flash 写保护检查都会从悬空指针读出
// 垃圾 address/size,把合法地址误判为"运行中 app 内"→ ESP_ERR_NOT_SUPPORTED
// → CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS 默认 abort()。
// 2026-10-04 真机:prepare 的记录扇区擦除 0x35A000 即因此 abort(栈上可见
// 0xa5a5a5a5 堆毒,decode:meta_carve_flash_commit ← h_install_prepare)。
// IDF 没有失效该缓存的 API;本固件运行中 app 恒为 factory(单机单会话,
// otadata 每次由 launcher 擦除),故用布局常量替代缓存查找。语义与 IDF
// 默认一致:护分区表扇区(≤0x9000 顺带盖住 bootloader)、factory、
// readonly 分区,其余放行。factory 范围与 meta_carve.c FIXED[0] 同步改。

#define CARVE_FACTORY_OFFSET 0x10000u
#define CARVE_FACTORY_END    (CARVE_FACTORY_OFFSET + 0x170000u)   // 0x180000

static esp_err_t carve_region_protected(void *arg, size_t start_addr, size_t size)
{
    (void)arg;   // os_func 钩子签名要求;设备/host 两侧都不使用(host -Werror 对齐)
    if (!esp_partition_is_flash_region_writable(start_addr, size)) {
        return ESP_ERR_NOT_ALLOWED;   // 与 IDF 一致:readonly 分区 → 调用方返回错误
    }
    if (start_addr <= ESP_PARTITION_TABLE_OFFSET + ESP_PARTITION_TABLE_MAX_LEN) {
        return ESP_ERR_NOT_SUPPORTED; // 分区表(及 bootloader) → abort 语义同 IDF
    }
    if (start_addr < CARVE_FACTORY_END && start_addr + size > CARVE_FACTORY_OFFSET) {
        return ESP_ERR_NOT_SUPPORTED; // 运行中 app(factory)
    }
    return ESP_OK;
}

// 一次性安装:默认主 flash chip 的 os_func 在 init 后不再更换
// (spi_flash_os_func_app.c 仅 init/deinit 路径赋值),拷贝一份静态表替换
// region_protected,其余钩子(start/end/缓存管理)原样保留。
static void carve_flash_wrap_region_protected(void)
{
    esp_flash_t *const chip = esp_flash_default_chip;
    if (!chip || !chip->os_func) return;
    static esp_flash_os_functions_t s_wrapped_os_func;
    s_wrapped_os_func = *chip->os_func;
    s_wrapped_os_func.region_protected = carve_region_protected;
    chip->os_func = &s_wrapped_os_func;
    ESP_LOGI(TAG, "flash region_protected wrapped (dangling ota cache bypassed)");
}

// ---- 裸 flash 原语 --------------------------------------------------------

static esp_err_t store_sector_read(uint32_t sector)
{
    return esp_flash_read(NULL, s_io,
                          META_STORE_OFFSET + sector * META_STORE_SECTOR_SIZE,
                          META_CARVE_REC_SIZE);
}

bool meta_carve_flash_table_read(uint8_t out[META_PT_SIZE])
{
    if (!out) return false;
    return esp_flash_read(NULL, out, META_PT_FLASH_OFFSET, META_PT_SIZE) == ESP_OK;
}

bool meta_carve_flash_reboot_pending(void)
{
    return s_reboot_pending;
}

// N21: 复位前显式清除,避免依赖 BSS 重置的隐式清零
void meta_carve_flash_reboot_clear(void)
{
    s_reboot_pending = false;
}

esp_err_t meta_carve_flash_table_write(const uint8_t table[META_PT_SIZE])
{
    if (!table) return ESP_ERR_INVALID_ARG;
    // 分区表扇区(0x8000)在 IDF 危险写保护清单内(bootloader / 分区表 /
    // 运行中 app 镜像区):esp_flash_* 顶层的 CHECK_WRITE_ADDRESS 命中即
    // abort()(CONFIG_SPI_FLASH_DANGEROUS_WRITE 默认 ABORTS)。
    // 2026-10-04 真机根因:prepare/删除路径的 commit(materialize) 在此
    // abort → 软件复位 → 手机请求悬挂、token 丢失、被迫重新配对。
    // 运行时物化 carved 表是本设计的合法操作(记录先行 + hook 下一开机
    // 校验修复,§4.7),这里只对这两次调用临时摘掉 region_protected 钩子,
    // start/end/缓存管理全部原样复用 IDF flash_ops。commit 的调用方都在
    // 同一 httpd 任务串行执行,无并发窗口。
    // M5: 防重入 —— 虽当前调用方串行,但加静态锁避免未来并发风险
    static bool s_in_progress = false;
    if (s_in_progress) return ESP_FAIL;
    s_in_progress = true;
    static esp_flash_os_functions_t s_pt_os_func;
    esp_flash_t *const chip = esp_flash_default_chip;
    s_pt_os_func = *chip->os_func;
    s_pt_os_func.region_protected = NULL;
    const esp_flash_os_functions_t *const saved_os = chip->os_func;
    chip->os_func = &s_pt_os_func;
    esp_err_t e = esp_flash_erase_region(chip, META_PT_FLASH_OFFSET,
                                         META_CARVE_SIZE_GRANULE);
    if (e == ESP_OK) {
        e = esp_flash_write(chip, table, META_PT_FLASH_OFFSET, META_PT_SIZE);
    }
    chip->os_func = saved_os;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "table sector erase/write failed: %s", esp_err_to_name(e));
        s_in_progress = false;
        return ESP_FAIL;
    }
    // 读回进 s_live:调用方传入的 table 不与 s_live 重叠(记录表在 s_best/s_tmp)。
    if (esp_flash_read(NULL, s_live, META_PT_FLASH_OFFSET, META_PT_SIZE) != ESP_OK ||
        memcmp(s_live, table, META_PT_SIZE) != 0) {
        ESP_LOGE(TAG, "table read-back mismatch");
        s_in_progress = false;
        return ESP_FAIL;
    }
    // 分区缓存失效:esp_partition 的 SRAM 缓存只在"首次访问"从 flash 表
    // 加载一次 —— 本 boot 若已加载(geom_refresh/槽扫描都触发),它持有的
    // ota_N→offset 映射是物化前的旧表;此时 chunk 写按 subtype 查缓存会
    // 拿到旧分区,镜像写进别人的槽(2026-10-04 真机:节拍器/混沌摆先后
    // 写进 leo-radio 的 0x360000,原应用被静默摧毁,记录与字节两处铁证)。
    // 物化是分区表变化的唯一漏斗,且全部调用方与在途 OTA 串行(单 httpd
    // 任务 + prepare 拒并发),此处 unload 后下次访问即从刚写好的表重建。
    esp_partition_unload_all();
    s_in_progress = false;
    return ESP_OK;
}

// 读 A/B,挑出最新且结构合法的记录(与 hook 同规则:解码 + 结构校验 +
// 有符号回绕 seq 比较)。*from_a 命中扇区(提交时轮转目标用)。
static bool load_best(meta_carve_rec_t *best, meta_carve_rec_t *tmp, bool *from_a)
{
    bool have = false;
    for (uint32_t i = 0; i < 2; i++) {
        if (store_sector_read(i) != ESP_OK) continue;
        if (!meta_carve_rec_decode(s_io, tmp) || !meta_carve_rec_validate(tmp)) {
            continue;
        }
        // 注意:不得引入局部大结构临时量(3584B 主栈)。
        if (!have || (int32_t)(tmp->seq - best->seq) > 0) {
            *best = *tmp;
            have = true;
            if (from_a) *from_a = (i == 0);
        }
    }
    return have;
}

// ---- 提交 -----------------------------------------------------------------


esp_err_t meta_carve_flash_materialize_active(uint32_t active_play_id)
{
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;

    // 0 = launcher view (APP only); non-zero = one Child Firmware's DATA.
    if (!meta_pt_from_carve_active(&s_carve, active_play_id, s_live)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (meta_carve_flash_table_read(s_ref) &&
        meta_pt_equal(s_ref, s_live)) {
        return ESP_OK;
    }
    return meta_carve_flash_table_write(s_live);
}

esp_err_t meta_carve_flash_commit(const meta_carve_t *carve, bool materialize)
{
    if (!carve) return ESP_ERR_INVALID_ARG;

    // 入参快照:调用方可能传 &s_best.carve / &s_carve(set_dirty / archive),
    // 而下方 load_best 会就地覆写 s_best —— 不先拷出来,改动会在写盘前被
    // 读回的旧值冲掉(表现为"重启即回滚")。快照后一律用 s_in。
    s_in = *carve;
    if (!meta_carve_valid(&s_in)) return ESP_ERR_INVALID_ARG;

    bool from_a = false;
    const bool have = load_best(&s_best, &s_tmp, &from_a);

    // 目标 = 非最新扇区(无记录 → A):先擦后写,旧记录在另一扇区兜底。
    const uint32_t target = have ? (from_a ? META_STORE_RECORD_B_SECTOR
                                           : META_STORE_RECORD_A_SECTOR)
                                 : META_STORE_RECORD_A_SECTOR;
    uint32_t seq = have ? s_best.seq + 1u : 1u;
    if (seq == 0u || seq == 0xFFFFFFFFu) seq = 1u;   // 回绕:1 新于 0xFFFFFFFE

    // 新记录借道 s_tmp(编码后 s_tmp 仍持有表字节,materialize 要用)。
    meta_carve_rec_t *rec = &s_tmp;
    rec->seq = seq;
    rec->carve = s_in;
    if (!meta_pt_from_carve(&s_in, rec->table)) return ESP_ERR_INVALID_ARG;
    if (!meta_carve_rec_encode(rec, s_io)) return ESP_ERR_INVALID_ARG;

    const uint32_t addr = META_STORE_OFFSET + target * META_STORE_SECTOR_SIZE;
    if (esp_flash_erase_region(NULL, addr, META_STORE_SECTOR_SIZE) != ESP_OK) {
        ESP_LOGE(TAG, "record sector %u erase failed", (unsigned)target);
        return ESP_FAIL;
    }
    const uint32_t wr_len = META_CARVE_REC_CRC_OFF + 4u;   // CRC 收尾,尾部保持擦除态
    if (esp_flash_write(NULL, s_io, addr, wr_len) != ESP_OK) {
        ESP_LOGE(TAG, "record sector %u write failed", (unsigned)target);
        return ESP_FAIL;
    }
    // 读回并"按未来读者的方式"重解码 + 结构校验(比 memcmp 更强)。
    if (store_sector_read(target) != ESP_OK) {
        ESP_LOGE(TAG, "record sector %u read-back failed", (unsigned)target);
        return ESP_FAIL;
    }
    if (!meta_carve_rec_decode(s_io, &s_best) || !meta_carve_rec_validate(&s_best) ||
        s_best.seq != seq) {
        ESP_LOGE(TAG, "record sector %u read-back invalid", (unsigned)target);
            return ESP_FAIL;
    }

    // 记录已落盘:采纳为规范状态。
    s_carve = s_in;
    s_best.carve = s_in;  // 同步 s_best
    s_seq = seq;
    s_have_record = true;

    // 表后写(记录先行):断电时 hook 下一开机从记录重建表(§4.7)。
    // 物化成功即挂起"退出商店页复位":运行时装了 carved 表/注册了外部
    // 分区,当前 boot 的分区缓存与 legacy 槽扫描都已过期,靠退出时重启
    // 清账;不立刻复位,保住手机侧会话 token(2026-10-04 交互修订)。
    if (materialize) {
        /*
         * "materialize" means the launcher runtime view, never the durable
         * full carve. Child DATA is exposed only by
         * meta_carve_flash_materialize_active() immediately before boot.
         */
        static uint8_t launcher_table[META_PT_SIZE];
        if (!meta_pt_from_carve_active(&s_in, 0, launcher_table)) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t e = meta_carve_flash_table_write(launcher_table);
        if (e != ESP_OK) return e;
        s_reboot_pending = true;
    }

    ESP_LOGI(TAG, "carve committed: seq=%lu slots=%u materialize=%d",
             (unsigned long)seq, (unsigned)s_in.count, (int)materialize);
    return ESP_OK;
}

// finalize 成功路径:把槽位在 carve 记录里就地晋升 VALID(镜像元数据
// 一并落记录)。新槽在 prepare 时以 EMPTY 占位,原靠"下次开机的
// sync_states 扫描晋升" —— 推迟复位修订后同一会话可能继续装第二个应用,
// 占位 EMPTY 会被手机清单当成空槽建议复用,第二个安装覆盖第一个(真机
// 2026-10-04)。几何未动:commit(materialize=false),不挂起复位。
esp_err_t meta_carve_flash_set_valid(int slot, const char *name,
                                     uint32_t image_len,
                                     const uint8_t sha256[32])
{
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;
    if (slot < 0 || slot >= (int)s_carve.count) return ESP_ERR_INVALID_ARG;
    s_work = s_carve;
    meta_carve_slot_t *s = &s_work.slot[slot];
    if (s->kind != META_CARVE_KIND_APP) return ESP_ERR_INVALID_ARG;
    s->state = (uint8_t)META_SLOT_VALID;
    s->image_len = image_len;
    memcpy(s->image_sha256, sha256, 32);
    if (name) {
        strncpy(s->name, name, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
    }
    return meta_carve_flash_commit(&s_work, false);
}

esp_err_t meta_carve_flash_remove(int slot)
{
    if (slot < 0 || slot >= (int)s_carve.count) return ESP_ERR_INVALID_ARG;

    meta_carve_t next = s_carve;
    if (!meta_carve_remove(&next, (uint8_t)slot)) return ESP_ERR_INVALID_ARG;
    // 记录先写、表后写(与 commit 同序):撕裂由 hook 下一开机修复。
    return meta_carve_flash_commit(&next, true);
}

/* Delete APP + all DATA records for its play_id with one durable metadata
 * commit. Callers erase extents before invoking this function, so a failed
 * commit leaves all extents reserved (even if their bytes are already erased). */
esp_err_t meta_carve_flash_remove_app_and_data(int slot)
{
    if (!s_active || slot < 0 || slot >= (int)s_carve.count) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t play_id = s_carve.slot[slot].play_id;
    s_work = s_carve;
    if (play_id != 0) {
        uint8_t i = 0;
        while (i < s_work.data_count) {
            if (s_work.data[i].play_id == play_id) {
                if (!meta_carve_remove_data(&s_work, i)) return ESP_ERR_INVALID_STATE;
                continue;
            }
            i++;
        }
    }
    if (!meta_carve_remove(&s_work, (uint8_t)slot)) return ESP_ERR_INVALID_ARG;
    return meta_carve_flash_commit(&s_work, true);
}
// ---- 扫描回填 -------------------------------------------------------------

static void hex32_to_bin(const char hex[META_SHA256_HEX_LEN], uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        const char h = hex[i * 2], l = hex[i * 2 + 1];
        const uint8_t hb = (uint8_t)(h <= '9' ? h - '0' : h - 'a' + 10);
        const uint8_t lb = (uint8_t)(l <= '9' ? l - '0' : l - 'a' + 10);
        out[i] = (uint8_t)((hb << 4) | lb);
    }
}

esp_err_t meta_carve_flash_sync_states(const meta_slot_info_t *slots, int count)
{
    if (!s_active || !slots) return ESP_ERR_INVALID_ARG;
    if (count < 0 || count > META_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_have_record) return ESP_OK;                  // 全新设备:尚无记录
    if (count != (int)s_carve.count) return ESP_ERR_INVALID_STATE;   // 同表派生

    meta_carve_t next = s_carve;
    bool changed = false;
    for (int i = 0; i < count; i++) {
        const meta_slot_info_t *info = &slots[i];
        meta_carve_slot_t built;
        memset(&built, 0, sizeof(built));
        built.kind = next.slot[i].kind;
        built.offset = next.slot[i].offset;
        built.size = next.slot[i].size;
        built.play_id = next.slot[i].play_id;   // 记录侧元数据,运行时表无此字段;
                                                // 丢失则卸载归档链 INVALID_STATE
                                                // (真机 S4 实锤:seq=6 同步后 play_id=0)
        built.state = (uint8_t)info->state;
        if (info->state == META_SLOT_VALID) {
            built.image_len = info->size;
            if (strlen(info->sha256_hex) == META_SHA256_HEX_LEN) {
                hex32_to_bin(info->sha256_hex, built.image_sha256);
            }
            size_t n = strlen(info->name);
            if (n > sizeof(built.name) - 1) n = sizeof(built.name) - 1;
            memcpy(built.name, info->name, n);
        }
        if (memcmp(&next.slot[i], &built, sizeof(built)) != 0) {
            next.slot[i] = built;
            changed = true;
        }
    }
    if (!changed) return ESP_OK;
    return meta_carve_flash_commit(&next, false);       // 状态不进表:免物化
}

// ---- 启动 ensure ----------------------------------------------------------

// L6 + dynslot:原裸 Wi-Fi 凭据备份(0x35A000)恰是 store 扇区0(记录A的地),
// 记录一写就会覆盖它 —— 先整扇区搬到 META_CRED_BAK_OFFSET(0x35E000,扇区4)。
// ensure 入口无条件跑(所有路径):
//   - 源头 4B 不是 MPCK(全 FF/记录/垃圾)→ 不动;
//   - 源头是 MPCK 且新家还没有 MPCK → 搬;新家已有 = 新 net.c 已写新家或
//     历次搬迁结果,保留较新者(也兼作幂等标记,重复 ensure 不擦不写);
// 源不擦:随后的记录写自会覆盖;凭据格式自校验,原样搬即可。
static void cred_relocate(void)
{
    uint32_t magic = 0;
    if (esp_flash_read(NULL, &magic, META_STORE_OFFSET, sizeof(magic)) != ESP_OK) {
        return;
    }
    if (magic != META_CRED_BAK_MAGIC) return;            // 不是旧凭据(FF/记录/垃圾)
    if (esp_flash_read(NULL, &magic, META_CRED_BAK_OFFSET, sizeof(magic)) == ESP_OK
        && magic == META_CRED_BAK_MAGIC) {
        return;                                          // 新家已有:保留较新者
    }
    if (esp_flash_read(NULL, s_io, META_STORE_OFFSET,
                       META_STORE_SECTOR_SIZE) != ESP_OK) {
        return;
    }
    if (esp_flash_erase_region(NULL, META_CRED_BAK_OFFSET,
                               META_STORE_SECTOR_SIZE) != ESP_OK) {
        ESP_LOGW(TAG, "cred relocate erase failed; old backup lost on record write");
        return;
    }
    if (esp_flash_write(NULL, s_io, META_CRED_BAK_OFFSET,
                        META_STORE_SECTOR_SIZE) != ESP_OK) {
        ESP_LOGW(TAG, "cred relocate write failed; old backup lost on record write");
        return;
    }
    ESP_LOGI(TAG, "wifi credential backup relocated to 0x%08lx",
             (unsigned long)META_CRED_BAK_OFFSET);
}

esp_err_t meta_carve_flash_ensure(void)
{
    if (s_active) return ESP_OK;

    // 首步:换掉 region_protected(见上方注释)。之后任何 flash 写检查都不再
    // 碰 esp_ota_get_running_partition() 的悬空缓存。
    carve_flash_wrap_region_protected();

    // 所有路径的第一步:扇区0 可能还站着旧凭据(有记录时也是 —— 旧 net.c
    // 曾把凭据写到那里)。MPCK 魔数守卫 + 新家幂等标记,非凭据内容不碰。
    cred_relocate();

    bool from_a = false;
    const bool have = load_best(&s_best, &s_tmp, &from_a);

    const bool live_ok = meta_carve_flash_table_read(s_live);

    if (have) {
        /*
         * The committed carve is the durable allocation authority.  The live
         * partition table is a runtime projection: launcher view contains no
         * Child DATA; a child view is materialized only immediately before its
         * boot.  app_main is always the launcher, so repair the live table to
         * launcher view rather than restoring the full carve table.
         */
        static uint8_t launcher_table[META_PT_SIZE];
        if (!meta_pt_from_carve_active(&s_best.carve, 0, launcher_table)) {
            ESP_LOGE(TAG, "cannot materialize launcher runtime table");
            return ESP_FAIL;
        }
        if (!live_ok || !meta_pt_equal(s_live, launcher_table)) {
            ESP_LOGW(TAG, "live table differs from launcher runtime view; re-materializing");
            const esp_err_t e = meta_carve_flash_table_write(launcher_table);
            if (e != ESP_OK) return e;
        }
        s_carve = s_best.carve;
        s_seq = s_best.seq;
        s_have_record = true;
        s_active = true;
        ESP_LOGI(TAG, "carve loaded: seq=%lu slots=%u",
                 (unsigned long)s_best.seq, (unsigned)s_best.carve.count);
        return ESP_OK;
    }

    if (!live_ok) {
        ESP_LOGE(TAG, "cannot read live table and no carve record");
        return ESP_FAIL;
    }

    meta_pt_safe(s_ref);
    if (meta_pt_equal(s_live, s_ref)) {
        // 全新设备(安全表):不预 carve —— 首次安装提案时才建槽,池全空,
        // 任意尺寸 first-fit 都放得下(design §1 的动机)。
        memset(&s_carve, 0, sizeof(s_carve));
        s_active = true;
        ESP_LOGI(TAG, "fresh device (safe table); no carve yet");
        return ESP_OK;
    }

    meta_pt_legacy(s_ref);
    if (meta_pt_equal(s_live, s_ref)) {
        // legacy v1.x 表 → 迁移(design §4.6):同偏移同尺寸种子,已装玩法不动。
        meta_pt_t t;   // 栈上 ~640B(3584B 主栈内)
        if (!meta_pt_decode(s_live, &t)) return ESP_FAIL;
        // 凭据已在 ensure 入口搬走(cred_relocate),记录写可安全覆盖扇区0。
        meta_carve_t seed;   // 栈上 ~740B
        if (!meta_carve_seed_legacy(&t, &seed)) {
            ESP_LOGE(TAG, "legacy table rejected by seed rules");
            return ESP_FAIL;
        }
        // 记录先行,表随后;两步都断电也由 hook 下一开机收敛(§4.7)。
        const esp_err_t e = meta_carve_flash_commit(&seed, true);
        if (e != ESP_OK) return e;
        s_active = true;
        ESP_LOGW(TAG, "migrated legacy table -> carve (%u slots, plays untouched)",
                 (unsigned)seed.count);
        return ESP_OK;
    }

    // 未知表 + 无记录:hook 应已恢复;会话内兜底写回内置安全表。
    ESP_LOGW(TAG, "unknown table without record; restoring safe table");
    meta_pt_safe(s_ref);
    const esp_err_t e = meta_carve_flash_table_write(s_ref);
    memset(&s_carve, 0, sizeof(s_carve));
    s_active = true;
    return e;
}


esp_err_t meta_carve_flash_set_dirty(uint32_t play_id)
{
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;
    if (play_id == 0) return ESP_ERR_INVALID_ARG;

    // 先在副本上翻转再提交:commit 会从 flash 重载 s_best,直接改 s_best 再
    // 提交会被读回的旧值覆盖(重启即回滚)。
    s_work = s_carve;
    bool changed = false;
    for (uint8_t i = 0; i < s_work.data_count; i++) {
        if (s_work.data[i].play_id == play_id &&
            s_work.data[i].state == META_DATA_PRISTINE) {
            s_work.data[i].state = META_DATA_DIRTY;
            changed = true;
        }
    }
    if (!changed) return ESP_OK;   // 幂等:已 DIRTY 或无数据记录
    return meta_carve_flash_commit(&s_work, false);
}

esp_err_t meta_carve_flash_mark_dirty_selected(const meta_carve_data_key_t *keys,
                                               uint8_t n_keys)
{
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;
    if (n_keys > 0 && !keys) return ESP_ERR_INVALID_ARG;

    s_work = s_carve;
    bool changed = false;
    for (uint8_t k = 0; k < n_keys; k++) {
        const int idx = meta_carve_find_data(&s_work, keys[k].play_id, keys[k].label);
        if (idx < 0) continue;
        if (s_work.data[idx].state == META_DATA_PRISTINE) {
            s_work.data[idx].state = META_DATA_DIRTY;
            changed = true;
        }
    }
    if (!changed) return ESP_OK;   // 幂等
    return meta_carve_flash_commit(&s_work, false);
}

esp_err_t meta_carve_flash_archive_slot_and_data(int slot)
{
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;
    if (slot < 0 || slot >= (int)s_carve.count) return ESP_ERR_INVALID_ARG;

    // 找该槽位的 play_id
    const uint32_t play_id = s_carve.slot[slot].play_id;
    if (play_id == 0) return ESP_ERR_INVALID_STATE;   // 旧记录无 play_id

    s_work = s_carve;
    bool changed = false;
    for (uint8_t i = 0; i < s_work.data_count; i++) {
        if (s_work.data[i].play_id == play_id) {
            s_work.data[i].state = META_DATA_ARCHIVED;
            changed = true;
        }
    }
    if (!changed) return ESP_OK;
    // 记录先行(不物化表:表条目由 caller 的 remove 后续提交)。
    return meta_carve_flash_commit(&s_work, false);
}

esp_err_t meta_carve_flash_erase_data(uint32_t play_id, const char *label)
{
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;
    if (play_id == 0) return ESP_ERR_INVALID_ARG;

    // 定位一条匹配记录(本函数一次只处理一条)。
    int idx = -1;
    for (uint8_t i = 0; i < s_carve.data_count; i++) {
        if (s_carve.data[i].play_id != play_id) continue;
        if (label && s_carve.data[i].label[0] != '\0' &&
            strcmp(s_carve.data[i].label, label) != 0) continue;
        idx = (int)i;
        break;
    }
    if (idx < 0) return ESP_OK;   // 无匹配:幂等 no-op

    const uint32_t off = s_carve.data[idx].offset;
    const uint32_t sz  = s_carve.data[idx].size;

    // 记录先行:先把条目移出记录并持久化,再擦字节 —— 断电最坏只残留一段
    // 已不被引用的空间(下次分配可复用),不会留下指向已擦区域的幽灵 carve。
    s_work = s_carve;
    if (!meta_carve_remove_data(&s_work, (uint8_t)idx)) return ESP_ERR_INVALID_ARG;
    const esp_err_t ce = meta_carve_flash_commit(&s_work, false);
    if (ce != ESP_OK) return ce;

    const esp_err_t e = esp_flash_erase_region(NULL, off, sz);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "erase data @0x%08lx failed: %s", (unsigned long)off,
                 esp_err_to_name(e));
        return e;
    }
    return ESP_OK;
}

static uint32_t arc_prepare_locked(uint32_t target)
{
    if (!s_active || !s_have_record || target == 0) return 0;

    s_work = s_carve;
    uint32_t reclaimed = 0;
    uint8_t i = 0;
    while (i < s_work.data_count && reclaimed < target) {
        if (s_work.data[i].state != META_DATA_ARCHIVED || s_work.data[i].size == 0) {
            i++;
            continue;
        }
        reclaimed += s_work.data[i].size;
        if (!meta_carve_remove_data(&s_work, i)) return 0;
    }
    if (reclaimed == 0) return 0;

    // Transactional callers must be able to roll the metadata back. Do not erase
    // the physical bytes here; they remain the rollback source until finalize.
    const esp_err_t ce = meta_carve_flash_commit(&s_work, false);
    return ce == ESP_OK ? reclaimed : 0;
}

uint32_t meta_carve_flash_arc_prepare(uint32_t target)
{
    return arc_prepare_locked(target);
}

uint32_t meta_carve_flash_arc(uint32_t target)
{
    if (!s_active || !s_have_record || target == 0) return 0;

    const meta_carve_t before = s_carve;
    const uint32_t reclaimed = arc_prepare_locked(target);
    if (reclaimed == 0) return 0;

    // Non-transactional legacy API: erase exactly the data records removed by
    // the metadata commit. The transactional install path calls arc_prepare()
    // and defers this erase until its final commit instead.
    const meta_carve_t *after = &s_carve;
    for (uint8_t i = 0; i < before.data_count; i++) {
        const meta_carve_data_t *old = &before.data[i];
        const int idx = meta_carve_find_data(after, old->play_id, old->label);
        if (idx >= 0 && after->data[idx].offset == old->offset) continue;
        (void)esp_flash_erase_region(NULL, old->offset, old->size);
    }
    return reclaimed;
}

