// tests/test_meta_carve_flash.c — dynslot 设备侧胶水的行为级 host 测试。
//
// 用 RAM NOR 模型(AND 写语义 + 擦除对齐校验 + 撕裂写注入)链接同一份
// meta_carve_flash.c,覆盖 design §4.7 失败矩阵在"启动器视角"的转换:
//   - 全新设备(安全表):不写记录、不改表;
//   - legacy 迁移:凭据先搬(L6)→ 记录先、表后 → 物化字节 == gen_esp32part
//     黄金产物 tests/fixtures/carve_migration_table.bin(逐字节);
//   - 记录在 + 表被改(断电/乱写)→ ensure 从记录重建表;
//   - store 死(双扇区坏)+ 表坏 → 回内置安全表;
//   - 提交撕裂写(断电)→ 读回重解码拒收、规范 carve 不变、轮转重试成功;
//   - 扫描回填 sync_states:状态/SHA/名字进记录、无变化不重写、count 不一致拒。
//
// esp_flash_* 在本文件实现(桩头 tests/esp_stubs/esp_flash.h 声明签名)。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_flash.h"
#include "meta_carve_flash.h"

#define FLASH_SIZE 0x800000u

static uint8_t s_flash[FLASH_SIZE];
static bool s_torn;
static uint32_t s_torn_limit;

esp_err_t esp_flash_read(void *chip, void *buffer, uint32_t address, uint32_t length)
{
    (void)chip;
    if ((uint64_t)address + length > FLASH_SIZE) return ESP_ERR_INVALID_ARG;
    memcpy(buffer, s_flash + address, length);
    return ESP_OK;
}

esp_err_t esp_flash_write(void *chip, const void *buffer, uint32_t address,
                          uint32_t length)
{
    (void)chip;
    if ((uint64_t)address + length > FLASH_SIZE) return ESP_ERR_INVALID_ARG;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t n = length;
    bool tear = false;
    if (s_torn && n > s_torn_limit) {
        n = s_torn_limit;
        tear = true;
    }
    for (uint32_t i = 0; i < n; i++) {
        s_flash[address + i] &= src[i];   // NOR:只能 1→0
    }
    if (tear) {
        s_torn = false;
        return ESP_FAIL;                  // 模拟写中断电
    }
    return ESP_OK;
}

esp_err_t esp_flash_erase_region(void *chip, uint32_t start_address, uint32_t size)
{
    (void)chip;
    if (start_address % 4096u || size % 4096u) return ESP_ERR_INVALID_ARG;
    if ((uint64_t)start_address + size > FLASH_SIZE) return ESP_ERR_INVALID_ARG;
    memset(s_flash + start_address, 0xFF, size);
    return ESP_OK;
}

// ---- 工具 -----------------------------------------------------------------

static void load_fixture(const char *path, uint8_t *dst, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL cannot open %s\n", path);
        assert(f);
    }
    assert(fread(dst, 1, n, f) == n);
    fclose(f);
}

static void reset_all(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    meta_carve_flash_test_reset();
    s_torn = false;
    s_torn_limit = 0;
}

// 模拟重启:清掉模块内存态后重走 ensure,从 flash 记录重载规范 carve。
// M5 状态翻转(set_dirty/archive/erase/arc)只有在重启后仍成立才算持久化 ——
// 只断言内存态会漏掉"commit 别名把改动写回旧值"这类"重启即回滚"缺陷。
static void restart(void)
{
    meta_carve_flash_test_reset();
    assert(meta_carve_flash_ensure() == ESP_OK);
}

static void table_is(const char *want_fix)
{
    uint8_t live[META_PT_SIZE];
    assert(meta_carve_flash_table_read(live));
    uint8_t want[META_PT_SIZE];
    load_fixture(want_fix, want, sizeof(want));
    if (memcmp(live, want, META_PT_SIZE) != 0) {
        fprintf(stderr, "FAIL live table != %s\n", want_fix);
        assert(0);
    }
}

// 读记录扇区(0=A,1=B)并解码。
static bool read_rec(uint32_t sector, meta_carve_rec_t *out)
{
    uint8_t raw[META_CARVE_REC_SIZE];
    const esp_err_t e = esp_flash_read(NULL, raw,
                                       META_STORE_OFFSET + sector * META_STORE_SECTOR_SIZE,
                                       sizeof(raw));
    return e == ESP_OK && meta_carve_rec_decode(raw, out) &&
           meta_carve_rec_validate(out);
}

// A/B 两扇区都读后按 seq 选取胜者(模拟启动器加载)。
static bool read_best(meta_carve_rec_t *out)
{
    uint8_t a[META_CARVE_REC_SIZE], b[META_CARVE_REC_SIZE];
    bool from_a = false;
    if (esp_flash_read(NULL, a, META_STORE_OFFSET, sizeof(a)) != ESP_OK) return false;
    if (esp_flash_read(NULL, b, META_STORE_OFFSET + META_STORE_SECTOR_SIZE,
                       sizeof(b)) != ESP_OK) return false;
    return meta_carve_rec_pick(a, b, out, &from_a);
}

// ---- 场景 -----------------------------------------------------------------

static void test_fresh(void)
{
    reset_all();
    load_fixture("tests/fixtures/safe_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);
    assert(!meta_carve_flash_has_record());
    assert(meta_carve_flash_carve()->count == 0);
    table_is("tests/fixtures/safe_table.bin");   // 全新设备表不被改动
    // store 扇区0 保持擦除(无记录、无凭据搬移)。
    for (int i = 0; i < 16; i++) {
        assert(s_flash[META_STORE_OFFSET + i] == 0xFF);
    }
    // 全新设备无记录:sync 是 no-op。
    meta_slot_info_t slots[META_SLOT_COUNT];
    memset(slots, 0, sizeof(slots));
    assert(meta_carve_flash_sync_states(slots, 0) == ESP_OK);
    printf("PASS fresh device\n");
}

static void test_legacy_migration(void)
{
    reset_all();
    load_fixture("tests/fixtures/legacy_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    // 旧裸凭据备份(0x35A000 = store 扇区0):前 64B 有数据,余擦除。
    // 前 4B = 真实备份格式的 MPCK 魔数 —— ensure 的 cred_relocate 只认它
    // (区分旧凭据 vs 记录/垃圾;全 FF 或非 MPCK 内容不搬)。
    uint8_t cred[64];
    for (int i = 0; i < 64; i++) cred[i] = (uint8_t)(0x40 + i);
    const uint32_t mpck = META_CRED_BAK_MAGIC;
    memcpy(cred, &mpck, sizeof(mpck));
    memcpy(s_flash + META_STORE_OFFSET, cred, sizeof(cred));

    assert(meta_carve_flash_ensure() == ESP_OK);
    assert(meta_carve_flash_has_record());

    // carve:同偏移同尺寸种子(§4.6)。
    const meta_carve_t *c = meta_carve_flash_carve();
    assert(c->count == 3);
    assert(c->slot[0].offset == 0x180000 && c->slot[0].size == 0x1D6000);
    assert(c->slot[1].offset == 0x360000 && c->slot[1].size == 0x200000);
    assert(c->slot[2].offset == 0x560000 && c->slot[2].size == 0x29E000);

    // 记录 A:seq=1,结构合法。
    meta_carve_rec_t rec;
    assert(read_rec(META_STORE_RECORD_A_SECTOR, &rec));
    assert(rec.seq == 1);

    // 物化表 == gen_esp32part 黄金产物(逐字节,MD5 marker 同 IDF)。
    table_is("tests/fixtures/carve_migration_table.bin");

    // L6:凭据备份先于记录搬进 store 扇区4(0x35E000),扇区0 让位给记录。
    uint8_t got[64];
    memcpy(got, s_flash + META_CRED_BAK_OFFSET, sizeof(got));
    assert(memcmp(got, cred, sizeof(cred)) == 0);
    // 扇区0 已被记录取代(整体不再是裸凭据镜像;逐字节比较会撞上 'C')==0x43)。
    assert(memcmp(s_flash + META_STORE_OFFSET, cred, sizeof(cred)) != 0);

    // 幂等:再次 ensure 不重复提交(seq 不变)。
    assert(meta_carve_flash_ensure() == ESP_OK);
    assert(read_rec(META_STORE_RECORD_A_SECTOR, &rec));
    assert(rec.seq == 1);
    printf("PASS legacy migration (golden table + cred relocate + idempotent)\n");
}

static void test_record_restores_torn_table(void)
{
    // 迁移后,表被撕裂/乱改(0x8000 单扇区写断电)→ 重启 ensure 从记录重建。
    uint8_t bad[META_PT_SIZE];
    load_fixture("tests/fixtures/carve_migration_table.bin", bad, sizeof(bad));
    bad[64] ^= 0x5A;
    memcpy(s_flash + META_PT_FLASH_OFFSET, bad, META_PT_SIZE);

    meta_carve_flash_test_reset();   // 模拟重启
    assert(meta_carve_flash_ensure() == ESP_OK);
    assert(meta_carve_flash_has_record());
    table_is("tests/fixtures/carve_migration_table.bin");
    printf("PASS record restores torn table on reboot\n");
}

static void test_store_dead_falls_back_to_safe(void)
{
    // store 双扇区损坏 + 表损坏 → ensure 回内置安全表(§4.7 store 死;重装恢复)。
    memset(s_flash + META_STORE_OFFSET, 0x5A, 2 * META_STORE_SECTOR_SIZE);
    uint8_t bad[META_PT_SIZE];
    load_fixture("tests/fixtures/safe_table.bin", bad, sizeof(bad));
    bad[40] ^= 0x01;
    memcpy(s_flash + META_PT_FLASH_OFFSET, bad, META_PT_SIZE);

    meta_carve_flash_test_reset();
    assert(meta_carve_flash_ensure() == ESP_OK);
    assert(!meta_carve_flash_has_record());
    assert(meta_carve_flash_carve()->count == 0);
    table_is("tests/fixtures/safe_table.bin");
    printf("PASS store dead -> safe table\n");
}

static void test_commit_rotation_and_torn_write(void)
{
    reset_all();
    load_fixture("tests/fixtures/safe_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);

    // 首次提交(fresh 首装):记录 A,seq=1,materialize。
    meta_carve_t c1;
    memset(&c1, 0, sizeof(c1));
    assert(meta_carve_place(&c1, 0x20000, META_CARVE_KIND_APP, &c1) == 0);
    assert(meta_carve_flash_commit(&c1, true) == ESP_OK);
    meta_carve_rec_t rec;
    assert(read_rec(META_STORE_RECORD_A_SECTOR, &rec));
    assert(rec.seq == 1);
    assert(!read_rec(META_STORE_RECORD_B_SECTOR, &rec));   // B 仍擦除

    // 第二次提交撕裂写(写到 1000B 断电):读回重解码必须拒收,
    // 规范 carve 不得前进。
    meta_carve_t c2 = c1;
    assert(meta_carve_place(&c2, 0x20000, META_CARVE_KIND_APP, &c2) == 1);
    s_torn = true;
    s_torn_limit = 1000;
    assert(meta_carve_flash_commit(&c2, true) == ESP_FAIL);
    assert(meta_carve_flash_carve()->count == 1);          // 未采纳
    // A(旧 seq1)仍完好,B 撕裂。
    assert(read_rec(META_STORE_RECORD_A_SECTOR, &rec));
    assert(rec.seq == 1);
    assert(!read_rec(META_STORE_RECORD_B_SECTOR, &rec));

    // 重试:轮转仍写 B(非最新),seq 递进,物化生效。
    assert(meta_carve_flash_commit(&c2, true) == ESP_OK);
    assert(read_rec(META_STORE_RECORD_B_SECTOR, &rec));
    assert(rec.seq == 2);
    assert(read_rec(META_STORE_RECORD_A_SECTOR, &rec));    // 旧记录仍在兜底
    assert(meta_carve_flash_carve()->count == 2);
    // 表已物化为 2 槽:slot1@0x1A0000。
    uint8_t live[META_PT_SIZE];
    assert(meta_carve_flash_table_read(live));
    meta_pt_t t;
    assert(meta_pt_decode(live, &t));
    int apps = 0;
    for (int i = 0; i < t.count; i++) {
        if (t.e[i].type == 0 && t.e[i].subtype >= 0x10) apps++;
    }
    assert(apps == 2);
    printf("PASS commit rotation + torn-write rejection + retry\n");
}

static void test_sync_states(void)
{
    // 独立场景:legacy 迁移后记录已 committed(状态 EMPTY),扫描回填 VALID + 元数据。
    reset_all();
    load_fixture("tests/fixtures/legacy_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);

    meta_carve_rec_t before, after;
    assert(read_rec(META_STORE_RECORD_A_SECTOR, &before));
    assert(before.carve.slot[0].state == META_SLOT_EMPTY);

    meta_slot_info_t slots[META_SLOT_COUNT];
    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < 3; i++) {
        slots[i].state = META_SLOT_VALID;
        slots[i].size = 1000000u + (uint32_t)i;
        memset(slots[i].sha256_hex, 'a' + i, META_SHA256_HEX_LEN);
        snprintf(slots[i].name, sizeof(slots[i].name), "play-%d", i);
    }
    assert(meta_carve_flash_sync_states(slots, 3) == ESP_OK);
    assert(read_best(&after));
    assert(after.seq == before.seq + 1);
    assert(after.carve.slot[0].state == META_SLOT_VALID);
    assert(after.carve.slot[0].image_len == 1000000u);
    assert(after.carve.slot[0].name[0] == 'p');
    assert(after.carve.slot[1].state == META_SLOT_VALID);

    // 无变化 → 不重写(seq 不再前进;磨损友好)。
    meta_carve_rec_t cur = after;
    assert(meta_carve_flash_sync_states(slots, 3) == ESP_OK);
    assert(read_best(&after));
    assert(after.seq == cur.seq);

    // count 与规范 carve 不一致(不同表派生)→ 拒。
    assert(meta_carve_flash_sync_states(slots, 2) == ESP_ERR_INVALID_STATE);
    printf("PASS sync states (backfill / no-op / count mismatch)\n");
}

// §4.5 Remove:标记释放 + 表物化去条目(数据不搬移);不在 carve 的下标拒。
static void test_remove_slot(void)
{
    reset_all();
    load_fixture("tests/fixtures/legacy_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);

    meta_carve_rec_t before, after;
    assert(read_best(&before));
    assert(before.carve.count == 3);

    assert(meta_carve_flash_remove(1) == ESP_OK);
    const meta_carve_t *c = meta_carve_flash_carve();
    assert(c->count == 2);
    // 压缩数组:原 slot2(0x560000)顶上;数据不搬移(v1)。
    assert(c->slot[1].offset == 0x560000 && c->slot[1].size == 0x29E000);
    assert(read_best(&after));
    assert(after.seq == before.seq + 1);
    assert(after.carve.count == 2);

    // 表重新物化:池内仍留 0x180000 条目,0x360000 消失,APP 条目 = 2。
    uint8_t live[META_PT_SIZE];
    assert(meta_carve_flash_table_read(live));
    meta_pt_t t;
    assert(meta_pt_decode(live, &t));
    int apps = 0;
    bool has_pool0 = false, has_360 = false;
    for (int i = 0; i < t.count; i++) {
        if (t.e[i].type == 0 && t.e[i].subtype >= 0x10) apps++;
        if (t.e[i].type == 0 && t.e[i].offset == 0x180000) has_pool0 = true;
        if (t.e[i].type == 0 && t.e[i].offset == 0x360000) has_360 = true;
    }
    assert(apps == 2 && has_pool0 && !has_360);

    // 不在 carve 的下标 → 拒,且不动记录(seq 不再前进)。
    assert(meta_carve_flash_remove(5) == ESP_ERR_INVALID_ARG);
    assert(read_best(&after));
    assert(after.seq == before.seq + 1);
    printf("PASS remove slot (carve shrinks + table re-materialized)\n");
}

// ---- M5 数据生命周期测试 -------------------------------------------------

static void make_rec_with_data(meta_carve_rec_t *rec, uint32_t play_id,
                               const char *label, uint32_t offset,
                               uint32_t size, uint8_t state)
{
    memset(rec, 0, sizeof(*rec));
    rec->seq = 1;
    rec->carve.count = 1;
    rec->carve.data_count = 1;
    rec->carve.slot[0].kind = META_CARVE_KIND_APP;
    rec->carve.slot[0].offset = 0x180000;
    rec->carve.slot[0].size = 0xF0000;
    rec->carve.slot[0].play_id = play_id;
    rec->carve.data[0].play_id = play_id;
    rec->carve.data[0].offset = offset;
    rec->carve.data[0].size = size;
    rec->carve.data[0].state = state;
    rec->carve.data[0].type = 1;   // DATA
    rec->carve.data[0].subtype = 1;  // non-OTA data type
    if (label) {
        strncpy(rec->carve.data[0].label, label,
                sizeof(rec->carve.data[0].label) - 1);
    }
    if (!meta_pt_from_carve(&rec->carve, rec->table)) {
        fprintf(stderr, "FAIL meta_pt_from_carve failed\n");
        fprintf(stderr, "  count=%u data_count=%u\n", rec->carve.count, rec->carve.data_count);
        fprintf(stderr, "  slot[0]: off=0x%06X size=0x%06X kind=%d state=%d\n",
                rec->carve.slot[0].offset, rec->carve.slot[0].size,
                rec->carve.slot[0].kind, rec->carve.slot[0].state);
        for (uint8_t i = 0; i < rec->carve.data_count; i++) {
            fprintf(stderr, "  data[%d]: off=0x%06X size=0x%06X state=%d type=%d subtype=%d\n",
                    i, rec->carve.data[i].offset, rec->carve.data[i].size,
                    rec->carve.data[i].state, rec->carve.data[i].type, rec->carve.data[i].subtype);
            fprintf(stderr, "    label='%s' len=%zu\n", rec->carve.data[i].label, strlen(rec->carve.data[i].label));
        }
        assert(0);
    }
}

static void test_set_dirty(void)
{
    reset_all();
    load_fixture("tests/fixtures/legacy_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);

    // 写入含数据的记录
    meta_carve_rec_t rec;
    make_rec_with_data(&rec, 123, "save", 0x280000, 0x1000,
                       META_DATA_PRISTINE);
    assert(meta_carve_flash_commit(&rec.carve, true) == ESP_OK);
    
    // set_dirty 应翻转为 DIRTY
    assert(meta_carve_flash_set_dirty(123) == ESP_OK);
    assert(meta_carve_flash_carve()->data[0].state == META_DATA_DIRTY);

    // 幂等:再次调用 no-op
    assert(meta_carve_flash_set_dirty(123) == ESP_OK);
    assert(meta_carve_flash_carve()->data[0].state == META_DATA_DIRTY);

    // play_id 不匹配 → 无变化
    assert(meta_carve_flash_set_dirty(999) == ESP_OK);
    assert(meta_carve_flash_carve()->data[0].state == META_DATA_DIRTY);

    // 持久化:重启后 flash 记录里的状态必须仍是 DIRTY(不能回滚到 PRISTINE)。
    restart();
    meta_carve_rec_t after;
    assert(read_best(&after));
    assert(after.carve.data_count == 1);
    assert(after.carve.data[0].play_id == 123);
    assert(after.carve.data[0].state == META_DATA_DIRTY);

    printf("PASS set_dirty (durable across restart)\n");
}

static void test_archive_slot_and_data(void)
{
    reset_all();
    load_fixture("tests/fixtures/legacy_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);

    meta_carve_rec_t rec;
    make_rec_with_data(&rec, 456, "content", 0x290000, 0x2000,
                       META_DATA_PRISTINE);
    assert(meta_carve_flash_commit(&rec.carve, true) == ESP_OK);

    // archive 槽位 0 的数据
    assert(meta_carve_flash_archive_slot_and_data(0) == ESP_OK);
    assert(meta_carve_flash_carve()->data[0].state == META_DATA_ARCHIVED);

    // 持久化:重启后记录里的状态必须仍是 ARCHIVED。
    restart();
    meta_carve_rec_t after;
    assert(read_best(&after));
    assert(after.carve.data_count == 1);
    assert(after.carve.data[0].play_id == 456);
    assert(after.carve.data[0].state == META_DATA_ARCHIVED);

    printf("PASS archive_slot_and_data (durable across restart)\n");
}

static void test_erase_data(void)
{
    reset_all();
    load_fixture("tests/fixtures/legacy_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);

    // 预写一些数据到池
    const uint32_t data_off = 0x280000;
    uint8_t data[0x1000];
    memset(data, 0xAA, sizeof(data));
    assert(esp_flash_write(NULL, data, data_off, sizeof(data)) == ESP_OK);

    meta_carve_rec_t rec;
    make_rec_with_data(&rec, 789, "save", data_off, 0x1000, META_DATA_DIRTY);
    assert(meta_carve_flash_commit(&rec.carve, true) == ESP_OK);

    // erase_data 应擦除池内字节并清除记录
    assert(meta_carve_flash_erase_data(789, "save") == ESP_OK);
    assert(meta_carve_flash_carve()->data_count == 0);

    // 池内字节已擦除
    uint8_t verify[0x1000];
    assert(esp_flash_read(NULL, verify, data_off, sizeof(verify)) == ESP_OK);
    for (uint32_t i = 0; i < sizeof(verify); i++) {
        assert(verify[i] == 0xFF);
    }

    // 持久化:重启后记录里该数据条目必须消失 —— 否则留下一条指向已擦区域的
    // 幽灵 carve(分配器仍视其为占用,且内容静默丢失)。
    restart();
    meta_carve_rec_t after;
    assert(read_best(&after));
    assert(meta_carve_find_data(&after.carve, 789, "save") < 0);

    printf("PASS erase_data (durable across restart)\n");
}

static void test_arc(void)
{
    reset_all();
    load_fixture("tests/fixtures/legacy_table.bin", s_flash + META_PT_FLASH_OFFSET,
                 META_PT_SIZE);
    assert(meta_carve_flash_ensure() == ESP_OK);

    // 写入一条归档数据
    meta_carve_rec_t rec;
    make_rec_with_data(&rec, 111, "a", 0x280000, 0x1000,
                       META_DATA_ARCHIVED);
    
    // 添加第二条数据
    rec.carve.data_count = 2;
    rec.carve.data[1].play_id = 222;
    rec.carve.data[1].offset = 0x290000;
    rec.carve.data[1].size = 0x1000;
    rec.carve.data[1].state = META_DATA_ARCHIVED;
    rec.carve.data[1].type = 1;
    rec.carve.data[1].subtype = 1;
    strncpy(rec.carve.data[1].label, "b", sizeof(rec.carve.data[1].label) - 1);

    assert(meta_carve_flash_commit(&rec.carve, true) == ESP_OK);

    // ARC 回收 0x1000 字节(应是最旧的)
    const uint32_t reclaimed = meta_carve_flash_arc(0x1000);
    assert(reclaimed == 0x1000);
    assert(meta_carve_flash_carve()->data_count == 1);
    assert(meta_carve_flash_carve()->data[0].play_id == 222);

    // 持久化:重启后回收结果必须保留 —— 否则回收在重启后被回滚,
    // 且被擦的字节与新记录不一致(分配器认为仍被占用)。
    restart();
    meta_carve_rec_t after;
    assert(read_best(&after));
    assert(after.carve.data_count == 1);
    assert(after.carve.data[0].play_id == 222);

    printf("PASS arc (durable across restart)\n");
}

int main(void)
{
    test_fresh();
    test_legacy_migration();
    test_record_restores_torn_table();
    test_store_dead_falls_back_to_safe();
    test_commit_rotation_and_torn_write();
    test_sync_states();
    test_remove_slot();
    test_set_dirty();
    test_archive_slot_and_data();
    test_erase_data();
    test_arc();
    printf("PASS test_meta_carve_flash\n");
    return 0;
}
