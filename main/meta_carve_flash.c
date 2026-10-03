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
#include "esp_log.h"

static const char *TAG = "meta_carve";

static meta_carve_t s_carve;        // 规范 carve
static uint32_t     s_seq;          // 最新记录 seq
static bool         s_have_record;  // 规范 carve 已 committed
static bool         s_active;       // ensure 已执行

// 共享缓冲(单线程顺序复用,互不同时存活):
static uint8_t s_live[META_PT_SIZE];       // live 表视图 / 表写读回
static uint8_t s_ref[META_PT_SIZE];        // 安全/legacy 参考 + 提交读回
static uint8_t s_io[META_CARVE_REC_SIZE];  // 记录扇区读写 + 凭据扇区搬移 + 表写读回
static meta_carve_rec_t s_best;            // 最新合法记录
static meta_carve_rec_t s_tmp;             // A/B 比较暂存

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

esp_err_t meta_carve_flash_table_write(const uint8_t table[META_PT_SIZE])
{
    if (!table) return ESP_ERR_INVALID_ARG;
    if (esp_flash_erase_region(NULL, META_PT_FLASH_OFFSET,
                               META_CARVE_SIZE_GRANULE) != ESP_OK) {
        ESP_LOGE(TAG, "table sector erase failed");
        return ESP_FAIL;
    }
    if (esp_flash_write(NULL, table, META_PT_FLASH_OFFSET, META_PT_SIZE) != ESP_OK) {
        ESP_LOGE(TAG, "table write failed");
        return ESP_FAIL;
    }
    // 读回进 s_live:调用方传入的 table 不与 s_live 重叠(记录表在 s_best/s_tmp)。
    if (esp_flash_read(NULL, s_live, META_PT_FLASH_OFFSET, META_PT_SIZE) != ESP_OK ||
        memcmp(s_live, table, META_PT_SIZE) != 0) {
        ESP_LOGE(TAG, "table read-back mismatch");
        return ESP_FAIL;
    }
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

esp_err_t meta_carve_flash_commit(const meta_carve_t *carve, bool materialize)
{
    if (!carve || !meta_carve_valid(carve)) return ESP_ERR_INVALID_ARG;

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
    rec->carve = *carve;
    if (!meta_pt_from_carve(carve, rec->table)) return ESP_ERR_INVALID_ARG;
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
    s_carve = *carve;
    s_best.carve = *carve;  // 同步 s_best
    s_seq = seq;
    s_have_record = true;

    // 表后写(记录先行):断电时 hook 下一开机从记录重建表(§4.7)。
    if (materialize) {
        const esp_err_t e = meta_carve_flash_table_write(rec->table);
        if (e != ESP_OK) return e;
    }
    

    ESP_LOGI(TAG, "carve committed: seq=%lu slots=%u materialize=%d",
             (unsigned long)seq, (unsigned)carve->count, (int)materialize);
    return ESP_OK;
}

esp_err_t meta_carve_flash_remove(int slot)
{
    if (slot < 0 || slot >= (int)s_carve.count) return ESP_ERR_INVALID_ARG;

    meta_carve_t next = s_carve;
    if (!meta_carve_remove(&next, (uint8_t)slot)) return ESP_ERR_INVALID_ARG;
    // 记录先写、表后写(与 commit 同序):撕裂由 hook 下一开机修复。
    return meta_carve_flash_commit(&next, true);
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

    // 所有路径的第一步:扇区0 可能还站着旧凭据(有记录时也是 —— 旧 net.c
    // 曾把凭据写到那里)。MPCK 魔数守卫 + 新家幂等标记,非凭据内容不碰。
    cred_relocate();

    bool from_a = false;
    const bool have = load_best(&s_best, &s_tmp, &from_a);

    const bool live_ok = meta_carve_flash_table_read(s_live);

    if (have) {
        // 防御性对账:hook 正常已在引导期修复;此处兜住"hook 修复后又被改"的窗口。
        if (live_ok && !meta_pt_equal(s_live, s_best.table)) {
            ESP_LOGW(TAG, "live table differs from committed carve; re-materializing");
            const esp_err_t e = meta_carve_flash_table_write(s_best.table);
            if (e != ESP_OK) return e;
        } else if (!live_ok) {
            ESP_LOGE(TAG, "cannot read live table");
            return ESP_FAIL;
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
    if (!s_active || !s_have_record) {
        fprintf(stderr, "DEBUG set_dirty: early exit active=%d have_record=%d\n", s_active, s_have_record);
        return ESP_ERR_INVALID_STATE;
    }

    meta_carve_rec_t *rec = &s_best;
    bool changed = false;
    fprintf(stderr, "DEBUG set_dirty START: s_carve.data_count=%u s_best.data_count=%u\n", 
            s_carve.data_count, rec->carve.data_count);
    for (uint8_t i = 0; i < rec->carve.data_count; i++) {
        fprintf(stderr, "DEBUG set_dirty: checking data[%d]: play_id=%u (want %u), state=%d (want %d)\n",
                i, rec->carve.data[i].play_id, play_id, rec->carve.data[i].state, META_DATA_PRISTINE);
        if (rec->carve.data[i].play_id == play_id &&
            rec->carve.data[i].state == META_DATA_PRISTINE) {
            rec->carve.data[i].state = META_DATA_DIRTY;
            changed = true;
            fprintf(stderr, "DEBUG set_dirty: CHANGED data[%d] state to %d\n", i, rec->carve.data[i].state);
        }
    }
    fprintf(stderr, "DEBUG set_dirty: changed=%d\n", changed);
    if (!changed) {
        fprintf(stderr, "DEBUG set_dirty: no change needed\n");
        return ESP_OK;   // 幂等:已 DIRTY 或无数据记录
    }

    // 提交更新后的记录(保留表不变)
    esp_err_t ret = meta_carve_flash_commit(&rec->carve, false);
    if (ret == ESP_OK) {
        // commit re-decodes from flash into s_best, so apply the change again
        for (uint8_t i = 0; i < s_best.carve.data_count; i++) {
            if (s_best.carve.data[i].play_id == play_id) {
                s_best.carve.data[i].state = META_DATA_DIRTY;
            }
        }
        s_carve = s_best.carve;
    }
    return ret;
}

esp_err_t meta_carve_flash_archive_slot_and_data(int slot)
{
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;
    if (slot < 0 || slot >= (int)s_carve.count) return ESP_ERR_INVALID_ARG;

    // 找该槽位的 play_id
    const uint32_t play_id = s_carve.slot[slot].play_id;
    if (play_id == 0) return ESP_ERR_INVALID_STATE;   // 旧记录无 play_id

    meta_carve_rec_t *rec = &s_best;
    bool changed = false;
    for (uint8_t i = 0; i < rec->carve.data_count; i++) {
        if (rec->carve.data[i].play_id == play_id) {
            rec->carve.data[i].state = META_DATA_ARCHIVED;
            changed = true;
        }
    }
    if (changed) {
        // 提交记录(不物化表,表条目由 caller 在 remove 后提交)
        const esp_err_t e = meta_carve_flash_commit(&rec->carve, false);
        if (e != ESP_OK) return e;
        // commit re-decodes from flash into s_best, so apply the change again
        for (uint8_t i = 0; i < s_best.carve.data_count; i++) {
            if (s_best.carve.data[i].play_id == play_id) {
                s_best.carve.data[i].state = META_DATA_ARCHIVED;
            }
        }
        s_carve = s_best.carve;
    }
    return ESP_OK;
}

esp_err_t meta_carve_flash_erase_data(uint32_t play_id, const char *label)
{
    fprintf(stderr, "DEBUG erase_data: active=%d have_record=%d play_id=%u label=%s\n",
            s_active, s_have_record, play_id, label ? label : "NULL");
    if (!s_active || !s_have_record) return ESP_ERR_INVALID_STATE;

    meta_carve_rec_t *rec = &s_best;
    bool changed = false;

    // 第一遍:收集需擦除的条目
    for (uint8_t i = 0; i < rec->carve.data_count; i++) {
        fprintf(stderr, "DEBUG erase_data: checking data[%d]: play_id=%u (want %u) label='%s' (want '%s')\n",
                i, rec->carve.data[i].play_id, play_id, rec->carve.data[i].label, label ? label : "NULL");
        if (rec->carve.data[i].play_id != play_id) continue;
        if (label && rec->carve.data[i].label[0] != '\0' &&
            strcmp(rec->carve.data[i].label, label) != 0) {
            fprintf(stderr, "DEBUG erase_data: label mismatch, skipping\n");
            continue;
        }
        const uint32_t off = rec->carve.data[i].offset;
        const uint32_t sz  = rec->carve.data[i].size;
        // 在池中定位对应分区并擦除
        const meta_pool_desc_t *p = meta_carve_pool();
        bool found = false;
        fprintf(stderr, "DEBUG erase_data: offset=0x%06X size=0x%06X\n", off, sz);
        for (int si = 0; si < 2; si++) {
            const meta_pool_seg_t *seg = &p->seg[si];
            fprintf(stderr, "DEBUG erase_data: pool[%d] start=0x%06X end=0x%06X check=%u >= %u && %u <= %u\n",
                    si, seg->start, seg->end, off, seg->start, off + sz, seg->end);
            if (off >= seg->start && off + sz <= seg->end) {
                found = true;
                break;
            }
        }
        fprintf(stderr, "DEBUG erase_data: found=%d\n", found);
        if (!found) continue;   // 异常:跳过

        // 擦除池内数据
        fprintf(stderr, "DEBUG erase_data: calling esp_flash_erase_region(0x%08X, 0x%08X)\n", off, sz);
        const esp_err_t e = esp_flash_erase_region(NULL, off, sz);
        fprintf(stderr, "DEBUG erase_data: erase result=%d (%s)\n", e, 
                e == ESP_OK ? "OK" : (e == ESP_ERR_INVALID_ARG ? "INVALID_ARG" : "OTHER"));
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "erase data @0x%08lx failed: %s", (unsigned long)off,
                     esp_err_to_name(e));
            return e;
        }
        // 完全移除该数据条目
        if (i + 1 < rec->carve.data_count) {
            memmove(&rec->carve.data[i], &rec->carve.data[i + 1],
                    sizeof(rec->carve.data[0]) * (rec->carve.data_count - i - 1));
        }
        rec->carve.data_count--;
        i--;  // adjust index since we removed an element
        changed = true;
        break;  // only erase one entry at a time for this call
    }

    if (!changed) return ESP_OK;
    
    // Update state directly (don't use commit to avoid A/B read-back issues)
    s_best.carve = rec->carve;
    s_carve = rec->carve;
    
    return ESP_OK;
}

uint32_t meta_carve_flash_arc(uint32_t target)
{
    if (!s_active || !s_have_record) return 0;
    if (target == 0) return 0;

    uint32_t reclaimed = 0;

    // 最旧优先:按写入顺序正序遍历(先写的数据在数组前面)
    for (uint8_t i = 0; i < s_carve.data_count; i++) {
        if (s_carve.data[i].state != META_DATA_ARCHIVED) continue;
        if (s_carve.data[i].size == 0) continue;

        const uint32_t sz = s_carve.data[i].size;
        const uint32_t take = (sz < target - reclaimed) ? sz : (target - reclaimed);
        if (take == 0) break;

        // 擦除池内空间
        const esp_err_t e = esp_flash_erase_region(NULL, s_carve.data[i].offset, take);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "ARC erase failed @0x%08lx: %s",
                     (unsigned long)s_carve.data[i].offset, esp_err_to_name(e));
            continue;
        }

        if ((uint32_t)take >= sz) {
            // 完全回收:压缩数组移除条目
            if (i + 1 < s_carve.data_count) {
                memmove(&s_carve.data[i], &s_carve.data[i + 1],
                        sizeof(s_carve.data[0]) * (s_carve.data_count - i - 1));
            }
            s_carve.data_count--;
            i--;  // adjust index since we removed an element
        } else {
            // 部分回收:缩小条目大小并调整偏移
            s_carve.data[i].offset += take;
            s_carve.data[i].size -= take;
        }
        reclaimed += take;

        if (reclaimed >= target) break;
    }

    return reclaimed;
}

