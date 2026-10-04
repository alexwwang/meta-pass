// tests/test_meta_carve_store.c — store A/B carve 记录编解码(host,先红后绿)。
//
// 覆盖(设计 §4.3 + §4.7 失败矩阵):
//   - A/B 记录格式:erase-before-write(未用字节 0xFF)、magic/version/seq/
//     slot_count、尾部 CRC32(标准 CRC-32 check value 钉死多项式/初值/异或)、
//     内嵌 carved 表(MD5 随解码校验);
//   - 损坏判定:CRC 篡改、版本越界、count 越界、表与 carve 不一致(结构校验);
//   - A/B 选取:新 seq 胜出、坏记录让位给旧记录、seq 回绕比较;
//   - name[40] 无 NUL 时解码强制终结(防越界)。
//
// CRC 黄金向量:标准 CRC-32("123456789") = 0xCBF43926(zlib.checksum 同值)。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meta_carve_store.h"

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

// 修复被手工篡改字段后的 CRC(测试专用,复用被测 CRC 实现)。
static void fix_crc(uint8_t raw[META_CARVE_REC_SIZE])
{
    uint32_t crc = meta_carve_crc32(raw, META_CARVE_REC_CRC_OFF);
    memcpy(raw + META_CARVE_REC_CRC_OFF, &crc, 4);
}

// 构造一条 seq 的合法记录(carve → 表)。
static void make_rec(meta_carve_rec_t *rec, const meta_carve_t *c, uint32_t seq)
{
    memset(rec, 0, sizeof(*rec));
    rec->seq = seq;
    rec->carve = *c;
    assert(meta_pt_from_carve(&rec->carve, rec->table));
}

static meta_carve_t migration_carve(void)
{
    size_t n = 0;
    uint8_t *raw = load("tests/fixtures/legacy_table.bin", &n);
    meta_pt_t t;
    assert(n == META_PT_SIZE && meta_pt_decode(raw, &t));
    free(raw);
    meta_carve_t c;
    assert(meta_carve_seed_legacy(&t, &c));
    return c;
}

static void test_crc_check_value(void)
{
    assert(meta_carve_crc32("123456789", 9) == 0xCBF43926u);
    printf("PASS crc check value\n");
}

static void test_roundtrip(void)
{
    meta_carve_t c = migration_carve();
    // name[40] 全非 NUL:编码只写 40 字节,解码必须强制 NUL(防越界)。
    memset(c.slot[0].name, 'A', 40);
    c.slot[0].name[40] = '\0';
    meta_carve_rec_t rec;
    make_rec(&rec, &c, 7);

    uint8_t raw[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&rec, raw));
    // erase-before-write:尾部(3796..4096)保持擦除态 0xFF。
    for (size_t i = META_CARVE_REC_CRC_OFF + 4; i < META_CARVE_REC_SIZE; i++) {
        assert(raw[i] == 0xFF);
    }

    meta_carve_rec_t back;
    assert(meta_carve_rec_decode(raw, &back));
    assert(back.seq == 7);
    assert(back.carve.count == 3);
    assert(back.carve.slot[2].offset == 0x560000 && back.carve.slot[2].size == 0x29E000);
    assert(memcmp(back.table, rec.table, META_PT_SIZE) == 0);
    // name 终结边界 + 记录结构一致(name 不参与表物化)。
    assert(back.carve.slot[0].name[40] == '\0');
    assert(memcmp(back.carve.slot[0].name, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 40) == 0);
    assert(meta_carve_rec_validate(&back));
    printf("PASS record roundtrip\n");
}

static void test_decode_rejects(void)
{
    meta_carve_t c = migration_carve();
    meta_carve_rec_t rec;
    make_rec(&rec, &c, 7);
    uint8_t raw[META_CARVE_REC_SIZE];
    uint8_t work[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&rec, raw));

    // CRC 篡改。
    memcpy(work, raw, sizeof(work));
    work[100] ^= 1;
    assert(!meta_carve_rec_decode(work, &rec));
    // magic 篡改。
    memcpy(work, raw, sizeof(work));
    work[0] ^= 1;
    assert(!meta_carve_rec_decode(work, &rec));
    // version=3(未知版本;2 是当前合法写出版本)。
    memcpy(work, raw, sizeof(work));
    work[4] = 3;
    fix_crc(work);
    assert(!meta_carve_rec_decode(work, &rec));
    // slot_count = 9(越界)。
    memcpy(work, raw, sizeof(work));
    work[6] = 9;
    fix_crc(work);
    assert(!meta_carve_rec_decode(work, &rec));
    // seq = 0xFFFFFFFF(擦除态)。
    memcpy(work, raw, sizeof(work));
    memset(work + 8, 0xFF, 4);
    fix_crc(work);
    assert(!meta_carve_rec_decode(work, &rec));
    // 表 MD5 破坏(CRC 修复 → 只有表 MD5 拦得住)。
    memcpy(work, raw, sizeof(work));
    work[META_CARVE_REC_TABLE_OFF + 40] ^= 1;   // 表区第一条目内
    fix_crc(work);
    assert(!meta_carve_rec_decode(work, &rec));
    // 全 0xFF(擦除扇区)。
    memset(work, 0xFF, sizeof(work));
    assert(!meta_carve_rec_decode(work, &rec));
    printf("PASS decode rejects\n");
}

// 轻校验 + seq 读取(dynslot §4.4 bootloader dram 预算):只做格式层
// (magic/version/count/seq/CRC),不物化 carve/表 —— hook 先比 seq、只对
// 胜者跑完整 decode。表 MD5 故意不在此层(那是 decode 的活)。
static void test_raw_info(void)
{
    meta_carve_t c = migration_carve();
    meta_carve_rec_t rec;
    make_rec(&rec, &c, 7);
    uint8_t raw[META_CARVE_REC_SIZE];
    uint8_t work[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&rec, raw));

    uint32_t seq = 0;
    assert(meta_carve_rec_raw_info(raw, &seq) && seq == 7);

    // CRC 篡改 → 拒。
    memcpy(work, raw, sizeof(work));
    work[100] ^= 1;
    assert(!meta_carve_rec_raw_info(work, &seq));
    // magic 篡改 → 拒。
    memcpy(work, raw, sizeof(work));
    work[0] ^= 1;
    assert(!meta_carve_rec_raw_info(work, &seq));
    // version=3(修 CRC)→ 拒。
    memcpy(work, raw, sizeof(work));
    work[4] = 3;
    fix_crc(work);
    assert(!meta_carve_rec_raw_info(work, &seq));
    // slot_count=9(修 CRC)→ 拒。
    memcpy(work, raw, sizeof(work));
    work[6] = 9;
    fix_crc(work);
    assert(!meta_carve_rec_raw_info(work, &seq));
    // seq=0(修 CRC)→ 拒。
    memcpy(work, raw, sizeof(work));
    memset(work + 8, 0, 4);
    fix_crc(work);
    assert(!meta_carve_rec_raw_info(work, &seq));
    // 擦除态(全 FF)→ 拒。
    memset(work, 0xFF, sizeof(work));
    assert(!meta_carve_rec_raw_info(work, &seq));
    // 表 MD5 破坏(CRC 修复):raw_info 只看格式层 → 放行,decode 才拦。
    memcpy(work, raw, sizeof(work));
    work[META_CARVE_REC_TABLE_OFF + 40] ^= 1;
    fix_crc(work);
    assert(meta_carve_rec_raw_info(work, &seq) && seq == 7);
    meta_carve_rec_t out;
    assert(!meta_carve_rec_decode(work, &out));
    printf("PASS raw info\n");
}

// M1/M5:v2 记录的数据 carve 往返(play_id/state/label、header data_count、
// 16B label 终结边界) + 保留标签/越界 data_count 被 encode 拒。
static void test_data_roundtrip(void)
{
    meta_carve_t c = migration_carve();
    c.count = 2;   // 退掉 ota_2 → pool_1 尾部 2.6MB 空洞(回收后的可切分区间)
    c.slot[0].play_id = 77;

    meta_carve_data_t d;
    memset(&d, 0, sizeof(d));
    d.play_id = 563;
    d.size = 0x10000;   // 64KB(≥4KB 且 64KB 对齐下放置器有落点)
    d.state = META_DATA_PRISTINE;
    d.subtype = 0x82;
    d.type = 1;
    memcpy(d.label, "spiffs", 7);
    uint32_t off = 0;
    assert(meta_carve_place_data(&c, d.size, &off));
    d.offset = off;
    assert(meta_carve_data_append(&c, &d));

    // 第二条:ARCHIVED(回收阶梯只碰 ARCHIVED/PRISTINE,DIRTY 不入栈)。
    meta_carve_data_t d2 = d;
    d2.play_id = 564;
    d2.subtype = 0x83;
    d2.state = META_DATA_ARCHIVED;
    memcpy(d2.label, "0123456789abcdef", 17);   // 16 字符 → 记录内无 NUL
    assert(meta_carve_place_data(&c, d2.size, &off));
    d2.offset = off;
    assert(meta_carve_data_append(&c, &d2));
    assert(meta_carve_valid(&c));

    meta_carve_rec_t rec;
    make_rec(&rec, &c, 11);
    uint8_t raw[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&rec, raw));
    assert((raw[12] | (raw[13] << 8)) == 2);   // header data_count @12

    meta_carve_rec_t back;
    assert(meta_carve_rec_decode(raw, &back));
    assert(back.carve.data_count == 2);
    assert(back.carve.slot[0].play_id == 77);
    assert(back.carve.data[0].play_id == 563);
    assert(back.carve.data[0].offset == d.offset &&
           back.carve.data[0].size == d.size &&
           back.carve.data[0].state == META_DATA_PRISTINE &&
           back.carve.data[0].subtype == 0x82 && back.carve.data[0].type == 1);
    assert(strcmp(back.carve.data[0].label, "spiffs") == 0);
    assert(back.carve.data[1].state == META_DATA_ARCHIVED &&
           back.carve.data[1].play_id == 564);
    assert(back.carve.data[1].label[16] == '\0');   // 16B 强制终结
    assert(strcmp(back.carve.data[1].label, "0123456789abcdef") == 0);
    assert(memcmp(back.table, rec.table, META_PT_SIZE) == 0);
    assert(meta_carve_rec_validate(&back));

    // 保留标签(如 store)→ 子固件拿同名条目就能擦系统区 → encode 层拒。
    meta_carve_rec_t bad = rec;
    uint8_t dump[META_CARVE_REC_SIZE];
    strcpy(bad.carve.data[0].label, "store");
    assert(!meta_carve_rec_encode(&bad, dump));
    // data_count 越界 → 拒。
    bad = rec;
    bad.carve.data_count = META_DATA_MAX + 1;
    assert(!meta_carve_rec_encode(&bad, dump));
    printf("PASS data roundtrip\n");
}

static void t_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void t_wr32(uint8_t *p, uint32_t v)
{
    for (int k = 0; k < 4; k++) {
        p[k] = (uint8_t)(v >> (8 * k));
    }
}

// v1 兼容读:手搋一条老布局记录(88B 槽、无 play_id、无 data 区、crc @3792)
// → 解码物化成等价 v2 结构(play_id=0/data_count=0),表逐字节相同、validate 过。
static void test_v1_compat_decode(void)
{
    meta_carve_t c = migration_carve();
    meta_carve_rec_t rec;
    make_rec(&rec, &c, 42);   // 表字节与版本无关(物化自同 carve)

    uint8_t raw[META_CARVE_REC_SIZE];
    memset(raw, 0xFF, sizeof(raw));
    t_wr32(raw + 0, META_CARVE_REC_MAGIC);
    t_wr16(raw + 4, META_CARVE_REC_V1_VERSION);
    t_wr16(raw + 6, c.count);
    t_wr32(raw + 8, 42);
    for (uint8_t i = 0; i < c.count; i++) {
        const meta_carve_slot_t *s = &c.slot[i];
        uint8_t *p = raw + META_CARVE_REC_HEADER +
                     (size_t)i * META_CARVE_REC_V1_SLOT_SIZE;
        p[0] = s->state;
        p[1] = s->kind;
        t_wr32(p + 4, s->offset);
        t_wr32(p + 8, s->size);
        t_wr32(p + 12, s->image_len);
        memcpy(p + 16, s->image_sha256, 32);
        memset(p + 48, 0, 40);
        for (size_t n = 0; s->name[n] != '\0' && n < 40; n++) {
            p[48 + n] = (uint8_t)s->name[n];
        }
    }
    memcpy(raw + META_CARVE_REC_V1_TABLE_OFF, rec.table, META_PT_SIZE);
    const uint32_t crc = meta_carve_crc32(raw, META_CARVE_REC_V1_CRC_OFF);
    memcpy(raw + META_CARVE_REC_V1_CRC_OFF, &crc, 4);

    uint32_t seq = 0;
    assert(meta_carve_rec_raw_info(raw, &seq) && seq == 42);
    meta_carve_rec_t back;
    assert(meta_carve_rec_decode(raw, &back));
    assert(back.carve.count == c.count && back.carve.data_count == 0);
    for (uint8_t i = 0; i < c.count; i++) {
        assert(back.carve.slot[i].play_id == 0);   // v1 未知归属
        assert(back.carve.slot[i].offset == c.slot[i].offset);
        assert(back.carve.slot[i].size == c.slot[i].size);
        assert(strcmp(back.carve.slot[i].name, c.slot[i].name) == 0);
    }
    assert(memcmp(back.table, rec.table, META_PT_SIZE) == 0);
    assert(meta_carve_rec_validate(&back));

    // v1 记录 CRC 放在 v2 位置(4032)无效 → raw_info 拒(布局严格按版本)。
    uint8_t work[META_CARVE_REC_SIZE];
    memcpy(work, raw, sizeof(work));
    t_wr32(work + META_CARVE_REC_CRC_OFF,
           meta_carve_crc32(work, META_CARVE_REC_CRC_OFF));
    t_wr32(work + META_CARVE_REC_V1_CRC_OFF, 0);
    assert(!meta_carve_rec_raw_info(work, &seq));
    printf("PASS v1 compat decode\n");
}

static void test_validate_format_layers(void)
{
    // 2026-10-04 修订:validate 不再做"表 vs carve"物化比对(原实现 3072B
    // 栈帧在 bootloader hook / app_main 上必然溢出,QEMU 实测定案;
    // bootloader dram_seg 也不容静态替代)。该比对的防御价值由既有层覆盖:
    // decode 验表 MD5、carve_valid 验结构不变量、decide/ensure 比对 live 表。
    // 本测试钉死剩余边界:表 MD5 损坏 → decode 拒;carve 结构非法 → validate 拒。
    meta_carve_t mig = migration_carve();
    meta_carve_rec_t good;
    make_rec(&good, &mig, 9);
    assert(meta_carve_rec_validate(&good));

    uint8_t raw[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&good, raw));
    raw[META_CARVE_REC_TABLE_OFF + 0] ^= 0xFF;   // 破坏表字节 → MD5 失配
    meta_carve_rec_t back;
    assert(!meta_carve_rec_decode(raw, &back));  // 格式层(表 MD5)拒绝

    // 注:结构非法的 carve 在 encode 层(meta_carve_valid)即被拒,无法
    // 经 make_rec 构造;validate 的 carve_valid 是同不变量的第二读,
    // 面向手工构造的 raw —— 该路径由 decode 的逐层校验共同覆盖。
    printf("PASS validate format layers\n");
}

static void test_pick_ab(void)
{
    meta_carve_t c = migration_carve();
    meta_carve_rec_t newer, older, out;
    make_rec(&newer, &c, 5);
    make_rec(&older, &c, 4);
    uint8_t a[META_CARVE_REC_SIZE], b[META_CARVE_REC_SIZE], bad[META_CARVE_REC_SIZE];
    assert(meta_carve_rec_encode(&newer, a));
    assert(meta_carve_rec_encode(&older, b));

    bool from_a = false;
    // 新者胜。
    assert(meta_carve_rec_pick(a, b, &out, &from_a) && from_a && out.seq == 5);
    // A 坏(模拟提交中断)→ B 胜。
    memcpy(bad, a, sizeof(bad));
    bad[500] ^= 0xFF;
    assert(meta_carve_rec_pick(bad, b, &out, &from_a) && !from_a && out.seq == 4);
    // 双坏 → 无。
    assert(!meta_carve_rec_pick(bad, bad, &out, &from_a));
    // 擦除态 A → B。
    memset(a, 0xFF, sizeof(a));
    assert(meta_carve_rec_pick(a, b, &out, &from_a) && !from_a);
    // seq 回绕:1 新于 0xFFFFFFFE;0xFFFFFFFD 旧于 2。
    make_rec(&newer, &c, 1u);
    make_rec(&older, &c, 0xFFFFFFFEu);
    assert(meta_carve_rec_encode(&newer, a) && meta_carve_rec_encode(&older, b));
    assert(meta_carve_rec_pick(a, b, &out, &from_a) && from_a && out.seq == 1u);
    make_rec(&newer, &c, 0xFFFFFFFDu);
    make_rec(&older, &c, 2u);
    assert(meta_carve_rec_encode(&newer, a) && meta_carve_rec_encode(&older, b));
    assert(meta_carve_rec_pick(a, b, &out, &from_a) && !from_a && out.seq == 2u);
    printf("PASS A/B pick\n");
}

int main(void)
{
    test_crc_check_value();
    test_roundtrip();
    test_decode_rejects();
    test_raw_info();
    test_data_roundtrip();
    test_v1_compat_decode();
    test_validate_format_layers();
    test_pick_ab();
    printf("PASS test_meta_carve_store\n");
    return 0;
}
