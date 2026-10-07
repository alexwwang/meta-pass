// tests/test_meta_carve.c — dynslot 核心纯逻辑:host 测试先红后绿。
//
// 覆盖(设计 docs/assets/dynslot-design.md §8 验证计划):
//   1. 分配器:池序(pool_0 小段优先 → pool_1)、64KB 偏移对齐、4KB 尺寸粒度、
//      128KB 最小槽、8 槽数上限、空位复用、删除;
//   2. carve↔分区表物化:字节级对拍 gen_esp32part.py 黄金产物(tests/fixtures,
//      由 tests/fixtures/gen_fixtures.py 生成,MD5 marker 格式同 IDF);
//   3. 通用表编解码:解析/回写往返、MD5 校验、外来表(play 563 真机字节);
//   4. 迁移种子:legacy 3 槽表 → 同偏移同尺寸 carve(§4.6);安全表池扫描
//      shrink-wrap 种子(升级产物直接写安全表时,玩法镜像仍在池内)。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meta_carve.h"

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

static void expect_table(const char *fix, const uint8_t got[META_PT_SIZE])
{
    size_t n = 0;
    uint8_t *want = load(fix, &n);
    assert(n == META_PT_SIZE);
    if (memcmp(got, want, META_PT_SIZE) != 0) {
        for (size_t i = 0; i < META_PT_SIZE; i++) {
            if (got[i] != want[i]) {
                fprintf(stderr, "FAIL %s: first diff at 0x%zx: got %02x want %02x\n",
                        fix, i, got[i], want[i]);
                break;
            }
        }
        assert(0);
    }
    free(want);
}

static meta_carve_t empty_carve(void)
{
    meta_carve_t c;
    memset(&c, 0, sizeof(c));
    c.count = 0;
    return c;
}

// ---- 1. 分配器 -------------------------------------------------------------

static void test_need_rounding(void)
{
    // 最小槽 128KB 优先于尾 sector 对齐结果。
    assert(meta_carve_need(1) == META_CARVE_MIN_SLOT);
    assert(meta_carve_need(META_CARVE_MIN_SLOT - META_CARVE_TAIL) == META_CARVE_MIN_SLOT);
    // 超过最小槽:align4k(image_len + 4KB 尾)。
    assert(meta_carve_need(META_CARVE_MIN_SLOT - META_CARVE_TAIL + 1) == 0x21000);
    // align 后仍小于最小槽 → 最小槽胜出。
    assert(meta_carve_need(0x12345) == META_CARVE_MIN_SLOT);
    printf("PASS need rounding\n");
}

static void test_place_pool_order(void)
{
    // 空 carve:小请求落在 pool_0(小段优先,保 pool_1 连续大段 —— §4.1)。
    meta_carve_t c = empty_carve();
    meta_carve_t out;
    int idx = meta_carve_place(&c, 0x20000, META_CARVE_KIND_APP, &out);
    assert(idx == 0);
    assert(out.count == 1);
    assert(out.slot[0].offset == 0x180000);
    assert(out.slot[0].size == 0x20000);

    // 大请求(> pool_0 全部 0x1D6000)必须落 pool_1。
    meta_carve_t big = empty_carve();
    idx = meta_carve_place(&big, 0x1E0000, META_CARVE_KIND_APP, &out);
    assert(idx == 0);
    assert(out.slot[0].offset == 0x360000);

    // pool_0 连续小槽:64KB 对齐推进。
    meta_carve_t c2 = out; // 占了 pool_1 不影响 pool_0
    c2.count = 0;
    memset(c2.slot, 0, sizeof(c2.slot));
    idx = meta_carve_place(&c2, 0x30000, META_CARVE_KIND_APP, &out);
    assert(idx == 0 && out.slot[0].offset == 0x180000 && out.slot[0].size == 0x30000);
    idx = meta_carve_place(&out, 0x50000, META_CARVE_KIND_APP, &c2);
    assert(idx == 1 && c2.slot[1].offset == 0x1B0000 && c2.slot[1].size == 0x50000);
    // 占住 pool_0 全部 → 下一个槽必须 pool_1。
    meta_carve_t full0 = c2;
    // 把 pool_0 剩余空间吃光:直接手工塞满到 0x356000。
    full0.slot[1].size = (uint32_t)(0x356000u - 0x1B0000u);
    assert(meta_carve_valid(&full0));
    idx = meta_carve_place(&full0, 0x20000, META_CARVE_KIND_APP, &out);
    assert(idx == 2 && out.slot[idx].offset == 0x360000);
    printf("PASS place pool order\n");
}

static void test_place_limits(void)
{
    // 8 槽数上限:第 9 个必须拒绝。
    meta_carve_t c = empty_carve();
    meta_carve_t out = c;
    for (int i = 0; i < META_CARVE_MAX_SLOTS; i++) {
        int idx = meta_carve_place(&out, 0x20000, META_CARVE_KIND_APP, &out);
        assert(idx == i);
    }
    meta_carve_t over = out;
    assert(meta_carve_place(&over, 0x20000, META_CARVE_KIND_APP, &out) == -1);
    // 尺寸低于最小槽拒绝(place 也强制 min)。
    meta_carve_t e = empty_carve();
    assert(meta_carve_place(&e, 0x1000, META_CARVE_KIND_APP, &out) == -1);
    // 非 4KB 粒度拒绝。
    assert(meta_carve_place(&e, 0x21001, META_CARVE_KIND_APP, &out) == -1);
    printf("PASS place limits\n");
}

static void test_fit_reuse_remove(void)
{
    meta_carve_t c = empty_carve();
    meta_carve_t out;
    meta_carve_place(&c, 0x40000, META_CARVE_KIND_APP, &out);
    meta_carve_place(&out, 0x60000, META_CARVE_KIND_APP, &c);
    c.slot[0].state = META_SLOT_VALID;
    c.slot[1].state = META_SLOT_EMPTY;
    assert(c.count == 2);

    // 复用:首个满足 image_len+尾 ≤ size 的 APP 槽(状态不挡安装覆盖)。
    assert(meta_carve_find_fit(&c, 0x40000 - META_CARVE_TAIL) == 0);
    assert(meta_carve_find_fit(&c, 0x40000 - META_CARVE_TAIL + 1) == 1);
    assert(meta_carve_find_fit(&c, 0x60000 - META_CARVE_TAIL) == 1);
    assert(meta_carve_find_fit(&c, 0x60000 - META_CARVE_TAIL + 1) == -1);
    // storage 槽不可作为安装目标。
    c.slot[0].kind = META_CARVE_KIND_STORAGE;
    assert(meta_carve_find_fit(&c, 0x1000) == 1);
    c.slot[0].kind = META_CARVE_KIND_APP;

    // 删除中间项:压缩并保持 offset 升序,余下槽不动。
    assert(meta_carve_remove(&c, 0));
    assert(c.count == 1);
    assert(c.slot[0].offset == 0x1C0000); // 第二槽原位(首槽 0x180000/0x40000 尾)
    assert(meta_carve_valid(&c));
    assert(!meta_carve_remove(&c, 5)); // 越界
    printf("PASS fit/reuse/remove\n");
}

static void test_valid_rules(void)
{
    meta_carve_t c = empty_carve();
    meta_carve_t out;
    meta_carve_place(&c, 0x20000, META_CARVE_KIND_APP, &out);
    assert(meta_carve_valid(&out));

    meta_carve_t bad = out;
    bad.slot[0].offset = 0x181000; // 非 64KB 对齐
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.slot[0].size = 0x1000; // < min
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.slot[0].size = 0x20001; // 非 4KB 粒度
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.slot[0].offset = 0x10000; // 出池(撞 factory)
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.slot[0].offset = 0x35A000; // 落进 store 空隙(非池内)
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.slot[0].offset = 0x7F0000; // 尾部越过 pool_1 末端前仍需 ≥ min:7F0000+20000=810000 > 7FE000
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.slot[0].kind = 7; // 未知 kind
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.slot[0].state = 9; // 未知 state
    assert(!meta_carve_valid(&bad));
    bad = out;
    bad.count = META_CARVE_MAX_SLOTS + 1;
    assert(!meta_carve_valid(&bad));
    // 乱序(未按 offset 升序)。
    meta_carve_t two = out;
    meta_carve_place(&out, 0x20000, META_CARVE_KIND_APP, &two);
    meta_carve_t swapped = two;
    meta_carve_slot_t tmp = swapped.slot[0];
    swapped.slot[0] = swapped.slot[1];
    swapped.slot[1] = tmp;
    assert(two.count == 2 && meta_carve_valid(&two));
    assert(!meta_carve_valid(&swapped));
    // 重叠。
    meta_carve_t ov = two;
    ov.slot[1].offset = ov.slot[0].offset; // 撞
    assert(!meta_carve_valid(&ov));
    printf("PASS valid rules\n");
}

// ---- 2. carve↔分区表物化(黄金字节对拍) -----------------------------------

static void test_materialize_golden(void)
{
    uint8_t buf[META_PT_SIZE];

    // 安全表 == partitions.csv → gen_esp32part.py 产物。
    meta_pt_safe(buf);
    expect_table("tests/fixtures/safe_table.bin", buf);

    // legacy 固定 3 槽表(编译内置,hook 白名单/迁移检测用)。
    meta_pt_legacy(buf);
    expect_table("tests/fixtures/legacy_table.bin", buf);

    // 迁移种子 carve(同 legacy 偏移/尺寸)→ 物化 == +store 的 9 条表。
    meta_pt_t legacy;
    size_t n = 0;
    uint8_t *raw = load("tests/fixtures/legacy_table.bin", &n);
    assert(n == META_PT_SIZE);
    assert(meta_pt_decode(raw, &legacy));
    free(raw);
    meta_carve_t seed;
    assert(meta_carve_seed_legacy(&legacy, &seed));
    assert(seed.count == 3);
    assert(seed.slot[0].offset == 0x180000 && seed.slot[0].size == 0x1D6000);
    assert(seed.slot[1].offset == 0x360000 && seed.slot[1].size == 0x200000);
    assert(seed.slot[2].offset == 0x560000 && seed.slot[2].size == 0x29E000);
    assert(meta_pt_from_carve(&seed, buf));
    expect_table("tests/fixtures/carve_migration_table.bin", buf);

    // 非连续 shrink carve(0x180000/0x80000 + 0x360000/0x200000)。
    meta_carve_t shrunk = empty_carve();
    shrunk.count = 2;
    shrunk.slot[0].kind = META_CARVE_KIND_APP;
    shrunk.slot[0].offset = 0x180000;
    shrunk.slot[0].size = 0x80000;
    shrunk.slot[1].kind = META_CARVE_KIND_APP;
    shrunk.slot[1].offset = 0x360000;
    shrunk.slot[1].size = 0x200000;
    assert(meta_carve_valid(&shrunk));
    assert(meta_pt_from_carve(&shrunk, buf));
    expect_table("tests/fixtures/carve_shrunk_table.bin", buf);

    // 非法 carve 不物化。
    meta_carve_t bad = shrunk;
    bad.slot[0].offset = 0x180001;
    assert(!meta_pt_from_carve(&bad, buf));
    printf("PASS materialize golden\n");
}

// ---- 3. 表编解码往返 -------------------------------------------------------

static void test_codec_roundtrip(void)
{
    // play 563 真机表(外来布局:factory 3M、recovery@0x20、easter SPIFFS)可解码,
    // MD5 随 decode 校验,回写字节等价(尾部 0xFF 不变)。
    size_t n = 0;
    uint8_t *raw = load("tests/fixtures/play563_table.bin", &n);
    assert(n == META_PT_SIZE);
    meta_pt_t t;
    assert(meta_pt_decode(raw, &t));
    assert(t.count == 9);
    assert(strcmp(t.e[2].label, "factory") == 0 && t.e[2].size == 0x300000);
    assert(strcmp(t.e[8].label, "recovery") == 0 && t.e[8].subtype == 0x20);
    uint8_t re[META_PT_SIZE];
    meta_pt_encode(&t, re);
    assert(memcmp(re, raw, META_PT_SIZE) == 0);
    free(raw);

    // MD5 篡改 → 拒绝。
    raw = load("tests/fixtures/safe_table.bin", &n);
    uint8_t *bad = malloc(META_PT_SIZE);
    memcpy(bad, raw, META_PT_SIZE);
    // marker 起点 = 8 条 * 32 = 256;digest 在 +16。
    bad[256 + 16] ^= 1;
    assert(!meta_pt_decode(bad, &t));
    // 条目 magic 破坏 → 拒绝。
    memcpy(bad, raw, META_PT_SIZE);
    bad[32] ^= 1;
    assert(!meta_pt_decode(bad, &t));
    // 无 MD5 marker → 拒绝。
    memcpy(bad, raw, META_PT_SIZE);
    memset(bad + 8 * 32, 0xFF, 32);
    assert(!meta_pt_decode(bad, &t));
    free(bad);
    free(raw);

    // 往返:safe 解码→编码 字节等价。
    raw = load("tests/fixtures/safe_table.bin", &n);
    assert(meta_pt_decode(raw, &t));
    meta_pt_encode(&t, re);
    assert(memcmp(re, raw, META_PT_SIZE) == 0);
    free(raw);
    printf("PASS codec roundtrip\n");
}

// ---- 4. 迁移种子 -----------------------------------------------------------

static void test_seed_legacy(void)
{
    meta_pt_t t;
    size_t n = 0;
    uint8_t *raw = load("tests/fixtures/legacy_table.bin", &n);
    assert(meta_pt_decode(raw, &t));
    free(raw);
    meta_carve_t c;
    assert(meta_carve_seed_legacy(&t, &c));
    assert(c.count == 3);
    const int legacy_ota_entry[3] = { 3, 5, 6 };   // nvs/phy/factory/ota_0/cardid/ota_1/ota_2/otadata
    for (int i = 0; i < 3; i++) {
        assert(c.slot[i].kind == META_CARVE_KIND_APP);
        assert(c.slot[i].state == META_SLOT_EMPTY);
        assert(c.slot[i].offset == t.e[legacy_ota_entry[i]].offset);
        assert(c.slot[i].size == t.e[legacy_ota_entry[i]].size);
    }
    assert(meta_carve_valid(&c));

    // 安全表(无 ota 条目)→ 无种子可言。
    raw = load("tests/fixtures/safe_table.bin", &n);
    assert(meta_pt_decode(raw, &t));
    free(raw);
    assert(!meta_carve_seed_legacy(&t, &c));
    printf("PASS seed legacy\n");
}

static void test_seed_images_shrinkwrap(void)
{
    // 单镜像 shrink-wrap:槽 = max(128KB, align4k(len+4KB))。
    meta_pool_image_t imgs[] = {
        { .offset = 0x180000, .image_len = 46080 }, // 45KB → min slot 128KB
    };
    meta_carve_t c;
    assert(meta_carve_seed_images(imgs, 1, &c));
    assert(c.count == 1);
    assert(c.slot[0].offset == 0x180000);
    assert(c.slot[0].size == META_CARVE_MIN_SLOT);

    // 大镜像按实际尺寸(4KB 粒度,超过最小槽)。
    meta_pool_image_t big[] = {
        { .offset = 0x360000, .image_len = 0x1F123 },
    };
    assert(meta_carve_seed_images(big, 1, &c));
    assert(c.slot[0].size == 0x21000); // 0x1F123+0x1000 → align4k

    // 多镜像升序 + 不重叠;前槽收缩不吞下一个镜像。
    meta_pool_image_t multi[] = {
        { .offset = 0x180000, .image_len = 0x100000 },
        { .offset = 0x1A0000, .image_len = 0x5000 },   // 落在前槽理论 size 内?
    };
    // 0x100000+0x1000 → 0x101000 槽尾 = 0x180000+0x101000 = 0x281000 > 0x1A0000
    // → 与下一镜像冲突:必须夹到 0x1A0000-0x180000=0x20000,但仍 ≥ 需求?否 → 拒绝。
    assert(!meta_carve_seed_images(multi, 2, &c));

    // 合法双镜像。
    meta_pool_image_t ok[] = {
        { .offset = 0x180000, .image_len = 0x1F000 }, // need = max(128K, 0x20000) = 0x20000
        { .offset = 0x1A0000, .image_len = 0x30000 }, // 恰好接上
    };
    assert(meta_carve_seed_images(ok, 2, &c));
    assert(c.count == 2);
    assert(c.slot[0].size == META_CARVE_MIN_SLOT);
    assert(c.slot[1].offset == 0x1A0000 && c.slot[1].size == 0x31000);
    assert(meta_carve_valid(&c));

    // 非法输入:未对齐 / 出池 / 乱序 / 超 8。
    meta_pool_image_t mis[] = { { .offset = 0x181000, .image_len = 0x1000 } };
    assert(!meta_carve_seed_images(mis, 1, &c));
    meta_pool_image_t outp[] = { { .offset = 0x80000, .image_len = 0x1000 } };
    assert(!meta_carve_seed_images(outp, 1, &c));
    meta_pool_image_t desc[] = {
        { .offset = 0x360000, .image_len = 0x1000 },
        { .offset = 0x180000, .image_len = 0x1000 },
    };
    assert(!meta_carve_seed_images(desc, 2, &c));
    meta_pool_image_t nine[9];
    for (int i = 0; i < 9; i++) {
        nine[i].offset = 0x360000u + (uint32_t)i * 0x20000u;
        nine[i].image_len = 0x1000;
    }
    assert(!meta_carve_seed_images(nine, 9, &c));
    // 尾镜像放不下(需 128KB 但到池尾不足)。
    meta_pool_image_t tail[] = { { .offset = 0x7F0000, .image_len = 0x1000 } };
    assert(!meta_carve_seed_images(tail, 1, &c));
    printf("PASS seed images shrink-wrap\n");
}


static void test_active_data_materialization(void)
{
    meta_carve_t c = empty_carve();
    meta_carve_t next;

    // Different Child Firmware instances may use the same label/subtype.
    assert(meta_carve_place(&c, 0x20000, META_CARVE_KIND_APP, &next) == 0);
    next.slot[0].play_id = 100;
    c = next;
    assert(meta_carve_place(&c, 0x20000, META_CARVE_KIND_APP, &next) == 1);
    next.slot[1].play_id = 200;
    c = next;

    meta_carve_data_t a = {
        .play_id = 100, .offset = 0x1C0000, .size = 0x10000,
        .state = META_DATA_DIRTY, .subtype = 0x82, .type = 1,
    };
    strcpy(a.label, "storage");
    assert(meta_carve_data_append(&c, &a));

    meta_carve_data_t b = {
        .play_id = 200, .offset = 0x1D0000, .size = 0x10000,
        .state = META_DATA_DIRTY, .subtype = 2, .type = 1,
    };
    strcpy(b.label, "storage");
    assert(meta_carve_data_append(&c, &b));
    assert(meta_carve_valid(&c));

    uint8_t table[META_PT_SIZE];
    assert(meta_pt_from_carve_active(&c, 100, table));

    meta_pt_t t;
    assert(meta_pt_decode(table, &t));
    int storage_count = 0;
    for (uint8_t i = 0; i < t.count; i++) {
        if (strcmp(t.e[i].label, "storage") == 0) {
            storage_count++;
            assert(t.e[i].offset == a.offset);
            assert(t.e[i].size == a.size);
        }
    }
    assert(storage_count == 1);

    assert(meta_pt_from_carve_active(&c, 200, table));
    assert(meta_pt_decode(table, &t));
    storage_count = 0;
    for (uint8_t i = 0; i < t.count; i++) {
        if (strcmp(t.e[i].label, "storage") == 0) {
            storage_count++;
            assert(t.e[i].offset == b.offset);
            assert(t.e[i].size == b.size);
        }
    }
    assert(storage_count == 1);

    // A Child without data still gets a valid runtime partition table.
    assert(meta_pt_from_carve_active(&c, 300, table));
    assert(meta_pt_decode(table, &t));
    for (uint8_t i = 0; i < t.count; i++) {
        assert(strcmp(t.e[i].label, "storage") != 0);
    }

    // play_id 0 is the launcher view: APP partitions remain, Child DATA is hidden.
    assert(meta_pt_from_carve_active(&c, 0, table));
    assert(meta_pt_decode(table, &t));
    for (uint8_t i = 0; i < t.count; i++) {
        assert(strcmp(t.e[i].label, "storage") != 0);
    }
    printf("PASS active data materialization\n");
}

static void test_free_space(void)
{
    // 总池 = 两池之和(§4.1:6,766,592 B;列表页"剩余空间"的单一事实源)。
    assert(meta_carve_pool_total() == 0x1D6000u + 0x49E000u);
    assert(meta_carve_pool_total() == 6766592u);
    assert(meta_carve_free(NULL) == 0);

    meta_carve_t c = empty_carve();
    assert(meta_carve_free(&c) == meta_carve_pool_total());

    // 占用 = 槽尺寸之和(含尾 sector);放一个最小槽后剩余相应减少。
    meta_carve_t out;
    assert(meta_carve_place(&c, META_CARVE_MIN_SLOT, META_CARVE_KIND_APP, &out) == 0);
    assert(meta_carve_free(&out) == meta_carve_pool_total() - META_CARVE_MIN_SLOT);

    // legacy 三槽种子恰好吃光两池(等价于今天的固定布局)→ 剩余 0;
    // 此时安装仍可复用既有槽(find_fit),但不能新开槽 —— 必须先删一个玩。
    meta_pt_t t;
    size_t n = 0;
    uint8_t *raw = load("tests/fixtures/legacy_table.bin", &n);
    assert(meta_pt_decode(raw, &t));
    free(raw);
    meta_carve_t legacy;
    assert(meta_carve_seed_legacy(&t, &legacy));
    assert(meta_carve_free(&legacy) == 0);
    printf("PASS free space\n");
}

int main(void)
{
    test_need_rounding();
    test_place_pool_order();
    test_place_limits();
    test_fit_reuse_remove();
    test_valid_rules();
    test_materialize_golden();
    test_codec_roundtrip();
    test_seed_legacy();
    test_seed_images_shrinkwrap();
    test_free_space();
    test_active_data_materialization();
    printf("PASS test_meta_carve\n");
    return 0;
}
