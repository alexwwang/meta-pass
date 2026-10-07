// tests/test_meta_carve_boot.c — bootloader hook 表裁决纯逻辑(host,先红后绿)。
//
// 覆盖(设计 §4.4 流程 + §4.7 失败矩阵):
//   - 有合法 committed 记录:live == 记录表 → 放行;否则从记录重写(RESTORE_RECORD);
//   - 无记录:live ∈ {安全表, legacy 固定 3 槽表} → 放行;其他(子固件乱写、
//     损坏、半写)→ 写回内置安全表(RESTORE_SAFE);
//   - 记录格式合法但结构不一致(carve↔表不符)→ 视同无记录;
//   - 失败矩阵逐条:store 提交中断、表写中断、store 死、子固件乱写表。
//
// 注意:decision 只给动作,flash 写/擦在 hooks.c;RESTORE_* 隐含
// "擦 otadata + 回 factory"(静态门 tests/test_dynslot_hook_gate.py 钉住接线)。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meta_carve_boot.h"

static uint8_t *load(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL cannot open %s\n", path);
        assert(f);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static uint8_t *fixture(const char *name)
{
    size_t n = 0;
    uint8_t *b = load(name, &n);
    assert(n == META_PT_SIZE);
    return b;
}

static meta_carve_t seed_carve(void)
{
    size_t n = 0;
    uint8_t *raw = load("tests/fixtures/legacy_table.bin", &n);
    meta_pt_t t;
    assert(meta_pt_decode(raw, &t));
    free(raw);
    meta_carve_t c;
    assert(meta_carve_seed_legacy(&t, &c));
    return c;
}

static meta_boot_table_verdict_t decide(const uint8_t *live, const meta_carve_rec_t *rec,
                                       int active_slot)
{
    return meta_carve_boot_decide(live, rec, active_slot);
}

static void test_record_match_proceed(void)
{
    meta_carve_t c = seed_carve();
    meta_carve_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.seq = 3;
    rec.carve = c;
    assert(meta_pt_from_carve(&c, rec.table));

    // live == committed → 放行(零额外重启路径)。
    meta_boot_table_verdict_t v = decide(rec.table, &rec, -1);
    assert(v.action == META_BOOT_TABLE_PROCEED);
    printf("PASS record match proceeds\n");
}

static void test_runtime_views(void)
{
    meta_carve_t c = seed_carve();
    /* Keep one APP slot so pool_1 is available for the synthetic DATA record. */
    c.count = 1;
    /* Give slot 0 a child identity and a data allocation. */
    c.slot[0].play_id = 105;
    uint32_t data_off = 0;
    assert(meta_carve_place_data(&c, 0x10000, &data_off));
    c.data_count = 1;
    c.data[0].play_id = 105;
    c.data[0].offset = data_off;
    c.data[0].size = 0x10000;
    c.data[0].state = META_DATA_PRISTINE;
    c.data[0].type = 1;
    c.data[0].subtype = 0x82;
    strcpy(c.data[0].label, "storage");
    assert(meta_carve_valid(&c));

    meta_carve_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.seq = 11;
    rec.carve = c;
    assert(meta_pt_from_carve(&c, rec.table));

    uint8_t launcher[META_PT_SIZE];
    uint8_t child[META_PT_SIZE];
    assert(meta_pt_from_carve_active(&c, 0, launcher));
    assert(meta_pt_from_carve_active(&c, 105, child));

    meta_boot_table_verdict_t v = decide(launcher, &rec, -1);
    assert(v.action == META_BOOT_TABLE_PROCEED);
    v = decide(child, &rec, 0);
    assert(v.action == META_BOOT_TABLE_PROCEED);

    /* A child table must not be accepted as the cold-launcher view. */
    v = decide(child, &rec, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);
    assert(v.table);
    assert(meta_pt_equal(v.table, launcher));

    /* And the launcher view must not be accepted while resuming slot 0. */
    v = decide(launcher, &rec, 0);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);
    assert(v.table);
    assert(meta_pt_equal(v.table, child));
}

static void test_record_mismatch_restores(void)
{
    meta_carve_t c = seed_carve();
    meta_carve_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.seq = 3;
    rec.carve = c;
    assert(meta_pt_from_carve(&c, rec.table));

    // 子固件整体替换成 play 563 的表 → 从记录重写。
    uint8_t *child = fixture("tests/fixtures/play563_table.bin");
    meta_boot_table_verdict_t v = decide(child, &rec, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);
    assert(v.reason && v.reason[0]);
    free(child);

    // 半写的表(表写中断)→ 从记录重写。
    uint8_t torn[META_PT_SIZE];
    memcpy(torn, rec.table, META_PT_SIZE);
    torn[64] ^= 0x5A;
    v = decide(torn, &rec, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);

    // 迁移窗口:记录已提交、表还是 legacy → hook 直接替启动器补物化。
    uint8_t *legacy = fixture("tests/fixtures/legacy_table.bin");
    v = decide(legacy, &rec, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);
    free(legacy);
    printf("PASS record mismatch restores\n");
}

static void test_no_record(void)
{
    // 安全表 → 放行(新设备冷启动、store 死后已回退的设备)。
    uint8_t *safe = fixture("tests/fixtures/safe_table.bin");
    meta_boot_table_verdict_t v = decide(safe, NULL, -1);
    assert(v.action == META_BOOT_TABLE_PROCEED);

    // legacy 表 → 放行(旧设备首启 dynslot,迁移尚未发生)。
    uint8_t *legacy = fixture("tests/fixtures/legacy_table.bin");
    v = decide(legacy, NULL, -1);
    assert(v.action == META_BOOT_TABLE_PROCEED);
    free(legacy);

    // 子固件乱写表 + store 死 → 回内置安全表。
    uint8_t *child = fixture("tests/fixtures/play563_table.bin");
    v = decide(child, NULL, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_SAFE);
    assert(v.reason && v.reason[0]);
    free(child);

    // 安全表单 bit 损坏 → RESTORE_SAFE。
    uint8_t corrupt[META_PT_SIZE];
    memcpy(corrupt, safe, META_PT_SIZE);
    corrupt[40] ^= 1;
    v = decide(corrupt, NULL, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_SAFE);
    free(safe);
    printf("PASS no record\n");
}

static void test_invalid_record_treated_as_absent(void)
{
    // 2026-10-04 契约修订:validate 不再做"表 vs carve"物化比对(3KB 栈帧在
    // bootloader hook / app_main 上必然溢出,QEMU 实测定案;bootloader
    // dram_seg 也无静态替代余量)。跨一致性由既有层覆盖:记录 CRC 同时覆盖
    // carve 与表(部分写/撕写必然败 CRC → 视同无记录,仍走白名单),内嵌表
    // MD5 保表字节自洽 —— 表↔carve 互相矛盾只能源于软件 bug 写坏记录,
    // 该防御由"写方自证"承担。此处验证残留语义:表↔carve 不一致的记录
    // 按 committed 权威处理(RESTORE_RECORD,写回其内部合法的表),
    // 而非整机回退安全表(避免把可恢复态打成"池内容不可达")。
    meta_carve_t mig = seed_carve();
    meta_carve_t shrunk = mig;
    shrunk.count = 1;
    shrunk.slot[0].size = 0x80000;
    assert(meta_carve_valid(&shrunk));
    meta_carve_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.seq = 9;
    rec.carve = mig;                  // 记录说 3 槽
    assert(meta_pt_from_carve(&shrunk, rec.table));  // 表却是 1 槽
    uint8_t *safe = fixture("tests/fixtures/safe_table.bin");
    meta_boot_table_verdict_t v = decide(safe, &rec, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);
    assert(v.table == rec.table);
    uint8_t *child = fixture("tests/fixtures/play563_table.bin");
    v = decide(child, &rec, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);
    free(safe);
    free(child);
    printf("PASS inconsistent record = committed authority (CRC/MD5 guard writes)\n");
}

// 失败矩阵(§4.7)逐条转换。
static void test_failure_matrix(void)
{
    meta_carve_t c = seed_carve();
    meta_carve_rec_t good;
    memset(&good, 0, sizeof(good));
    good.seq = 4;
    good.carve = c;
    assert(meta_pt_from_carve(&c, good.table));
    uint8_t b[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&good, b));

    // 1) mid store commit:A 撕裂,B(旧 seq)仍有效 → 选 B;live==B 表 → 放行。
    meta_carve_rec_t older = good;
    older.seq = 3;
    uint8_t older_b[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&older, older_b));
    uint8_t torn[META_CARVE_REC_SIZE];
    memcpy(torn, b, sizeof(torn));
    torn[900] ^= 0xFF;   // A 半写损坏
    meta_carve_rec_t picked;
    bool from_a = false;
    assert(meta_carve_rec_pick(torn, older_b, &picked, &from_a));
    assert(!from_a && picked.seq == 3);
    meta_boot_table_verdict_t v = decide(picked.table, &picked, -1);
    assert(v.action == META_BOOT_TABLE_PROCEED);

    // 2) mid table write:live 半写 + 有效记录 → RESTORE_RECORD。
    uint8_t half[META_PT_SIZE];
    memcpy(half, good.table, META_PT_SIZE);
    memset(half + 128, 0x00, 64);   // 单扇区写到一半的典型形态
    v = decide(half, &good, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);

    // 3) store 死(双记录坏)+ live 损坏 → RESTORE_SAFE(池内容不寻址,重装恢复 L7)。
    uint8_t dead[META_CARVE_REC_SIZE];
    memcpy(dead, b, sizeof(dead));
    dead[50] ^= 0xFF;
    meta_carve_rec_t out;
    assert(!meta_carve_rec_pick(dead, dead, &out, &from_a));
    uint8_t *corrupt = fixture("tests/fixtures/safe_table.bin");
    corrupt[100] ^= 0x01;
    v = decide(corrupt, NULL, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_SAFE);
    free(corrupt);

    // 4) 子固件乱写池/表:表被换成 play 563 → RECORD 或 SAFE 兜底(视记录存活)。
    uint8_t *child = fixture("tests/fixtures/play563_table.bin");
    v = decide(child, &good, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_RECORD);
    v = decide(child, NULL, -1);
    assert(v.action == META_BOOT_TABLE_RESTORE_SAFE);
    free(child);
    printf("PASS failure matrix\n");
}

int main(void)
{
    test_record_match_proceed();
    test_runtime_views();
    test_record_mismatch_restores();
    test_no_record();
    test_invalid_record_treated_as_absent();
    test_failure_matrix();
    printf("PASS test_meta_carve_boot\n");
    return 0;
}
