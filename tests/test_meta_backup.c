// tests/test_meta_backup.c — M5 备份格式单元测试
//
// 覆盖设计 §12.1:
//   - header 验证(magic/version/play_id/firmware_version/data_count)
//   - 严格 firmware_version 匹配
//   - serialize/parse roundtrip
//   - edge cases(truncated, oversized, empty label)

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_backup.h"

// ---- 测试辅助 ----

static void assert_true(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        assert(0);
    }
}

static void assert_false(bool cond, const char *msg)
{
    if (cond) {
        fprintf(stderr, "FAIL: %s (expected false)\n", msg);
        assert(0);
    }
}

static void assert_eq(uint32_t a, uint32_t b, const char *msg)
{
    if (a != b) {
        fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", msg, b, a);
        assert(0);
    }
}

static void assert_str_equal(const char *a, const char *b, const char *msg)
{
    if (strcmp(a, b) != 0) {
        fprintf(stderr, "FAIL: %s (expected '%s', got '%s')\n", msg, b, a);
        assert(0);
    }
}

// ---- 测试用例 ----

static void test_header_valid(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = META_BACKUP_MAGIC;
    h.version = META_BACKUP_VERSION;
    h.play_id = 123;
    strncpy(h.firmware_version, "1.2.3", sizeof(h.firmware_version) - 1);
    h.data_count = 2;

    assert_true(meta_backup_header_valid(&h), "valid header passes");
}

static void test_header_null(void)
{
    assert_false(meta_backup_header_valid(NULL), "null header rejected");
}

static void test_header_invalid_magic(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = 0xDEADBEEF;  // wrong magic
    h.version = META_BACKUP_VERSION;
    h.play_id = 123;
    strncpy(h.firmware_version, "1.2.3", sizeof(h.firmware_version) - 1);
    h.data_count = 0;

    assert_false(meta_backup_header_valid(&h), "invalid magic rejected");
}

static void test_header_invalid_version(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = META_BACKUP_MAGIC;
    h.version = 999;  // wrong version
    h.play_id = 123;
    strncpy(h.firmware_version, "1.2.3", sizeof(h.firmware_version) - 1);
    h.data_count = 0;

    assert_false(meta_backup_header_valid(&h), "invalid version rejected");
}

static void test_header_empty_play_id(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = META_BACKUP_MAGIC;
    h.version = META_BACKUP_VERSION;
    h.play_id = 0;  // invalid
    strncpy(h.firmware_version, "1.2.3", sizeof(h.firmware_version) - 1);
    h.data_count = 0;

    assert_false(meta_backup_header_valid(&h), "empty play_id rejected");
}

static void test_header_empty_version(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = META_BACKUP_MAGIC;
    h.version = META_BACKUP_VERSION;
    h.play_id = 123;
    h.firmware_version[0] = '\0';  // empty
    h.data_count = 0;

    assert_false(meta_backup_header_valid(&h), "empty firmware_version rejected");
}

static void test_header_too_many_records(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = META_BACKUP_MAGIC;
    h.version = META_BACKUP_VERSION;
    h.play_id = 123;
    strncpy(h.firmware_version, "1.2.3", sizeof(h.firmware_version) - 1);
    h.data_count = META_BACKUP_DATA_MAX + 1;  // too many

    assert_false(meta_backup_header_valid(&h), "too many records rejected");
}

static void test_firmware_match_exact(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = META_BACKUP_MAGIC;
    h.version = META_BACKUP_VERSION;
    h.play_id = 123;
    strncpy(h.firmware_version, "1.2.3", sizeof(h.firmware_version) - 1);
    h.data_count = 0;

    assert_true(meta_backup_firmware_match(&h, "1.2.3"), "exact version match");
}

static void test_firmware_match_mismatch(void)
{
    meta_backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = META_BACKUP_MAGIC;
    h.version = META_BACKUP_VERSION;
    h.play_id = 123;
    strncpy(h.firmware_version, "1.2.3", sizeof(h.firmware_version) - 1);
    h.data_count = 0;

    assert_false(meta_backup_firmware_match(&h, "1.3.0"), "version mismatch rejected");
    assert_false(meta_backup_firmware_match(&h, NULL), "NULL current version rejected");
    assert_false(meta_backup_firmware_match(&h, ""), "empty current version rejected");
}

static void test_serialize_basic(void)
{
    uint32_t play_ids[] = {456, 456, 789};
    uint8_t states[] = {2, 2, 1};  // 2=ARCHIVED, 1=DIRTY
    uint32_t offsets[] = {0x280000, 0x290000, 0x2A0000};
    uint32_t sizes[] = {0x1000, 0x2000, 0x1000};
    const char *labels[] = {"save", "content", "data"};
    int count = 3;

    meta_backup_data_t records[META_BACKUP_DATA_MAX];
    int written = meta_backup_serialize(play_ids, states, offsets, sizes, labels, count, records);

    assert_eq(written, 2, "serialized 2 archived records");
    assert_eq(records[0].play_id, 456, "record 0 play_id");
    assert_eq(records[0].offset, 0x280000, "record 0 offset");
    assert_eq(records[0].size, 0x1000, "record 0 size");
    assert_eq(records[0].state, 2, "record 0 state ARCHIVED");
    assert_str_equal(records[0].label, "save", "record 0 label");

    assert_eq(records[1].play_id, 456, "record 1 play_id");
    assert_eq(records[1].offset, 0x290000, "record 1 offset");
    assert_eq(records[1].size, 0x2000, "record 1 size");
    assert_str_equal(records[1].label, "content", "record 1 label");
}

static void test_serialize_no_archived(void)
{
    uint32_t play_ids[] = {456};
    uint8_t states[] = {1};  // DIRTY, not ARCHIVED
    uint32_t offsets[] = {0x280000};
    uint32_t sizes[] = {0x1000};
    const char *labels[] = {"save"};
    int count = 1;

    meta_backup_data_t records[META_BACKUP_DATA_MAX];
    int written = meta_backup_serialize(play_ids, states, offsets, sizes, labels, count, records);

    assert_eq(written, 0, "no archived records serialized");
}

static void test_parse_roundtrip(void)
{
    // Build backup data
    uint8_t buffer[256];
    memset(buffer, 0, sizeof(buffer));

    meta_backup_header_t *header = (meta_backup_header_t *)buffer;
    header->magic = META_BACKUP_MAGIC;
    header->version = META_BACKUP_VERSION;
    header->play_id = 123;
    strncpy(header->firmware_version, "2.0.0", sizeof(header->firmware_version) - 1);
    header->data_count = 2;

    meta_backup_data_t *rec0 = (meta_backup_data_t *)(buffer + sizeof(meta_backup_header_t));
    rec0->play_id = 123;
    rec0->offset = 0x280000;
    rec0->size = 0x1000;
    rec0->state = 2;  // ARCHIVED
    rec0->type = 1;
    rec0->subtype = 1;
    strncpy(rec0->label, "save", sizeof(rec0->label) - 1);

    meta_backup_data_t *rec1 = (meta_backup_data_t *)(buffer + sizeof(meta_backup_header_t) + sizeof(meta_backup_data_t));
    rec1->play_id = 123;
    rec1->offset = 0x290000;
    rec1->size = 0x2000;
    rec1->state = 2;  // ARCHIVED
    rec1->type = 1;
    rec1->subtype = 1;
    strncpy(rec1->label, "content", sizeof(rec1->label) - 1);

    size_t total_len = sizeof(meta_backup_header_t) + 2 * sizeof(meta_backup_data_t);

    // Parse
    meta_backup_result_t result = meta_backup_parse(buffer, total_len);
    assert_true(result.ok, "parse succeeded");
    assert_eq(result.play_id, 123, "play_id matches");
    assert_str_equal(result.firmware_version, "2.0.0", "version matches");
    assert_eq(result.data_count, 2, "record count matches");

    assert_eq(result.records[0].play_id, 123, "record 0 play_id");
    assert_eq(result.records[0].offset, 0x280000, "record 0 offset");
    assert_eq(result.records[0].size, 0x1000, "record 0 size");
    assert_eq(result.records[0].state, 2, "record 0 state");
    assert_str_equal(result.records[0].label, "save", "record 0 label");

    assert_eq(result.records[1].play_id, 123, "record 1 play_id");
    assert_eq(result.records[1].offset, 0x290000, "record 1 offset");
    assert_eq(result.records[1].size, 0x2000, "record 1 size");
    assert_str_equal(result.records[1].label, "content", "record 1 label");
}

static void test_parse_truncated(void)
{
    uint8_t buffer[32];
    memset(buffer, 0, sizeof(buffer));
    // Write partial header
    *(uint32_t *)buffer = META_BACKUP_MAGIC;

    meta_backup_result_t result = meta_backup_parse(buffer, 32);
    assert_false(result.ok, "truncated data rejected");
}

static void test_parse_bad_magic(void)
{
    uint8_t buffer[64];
    memset(buffer, 0, sizeof(buffer));
    *(uint32_t *)buffer = 0xDEADBEEF;  // wrong magic

    meta_backup_result_t result = meta_backup_parse(buffer, sizeof(buffer));
    assert_false(result.ok, "bad magic rejected");
}

static void test_filter_import(void)
{
    meta_backup_data_t in[4];
    memset(in, 0, sizeof(in));
    // [0] 合法 ARCHIVED
    in[0].play_id = 1; in[0].offset = 0x280000; in[0].size = 0x1000;
    in[0].state = 2; strcpy(in[0].label, "save");
    // [1] state=DIRTY → 剔除
    in[1].play_id = 1; in[1].offset = 0x290000; in[1].size = 0x1000;
    in[1].state = 1; strcpy(in[1].label, "a");
    // [2] ARCHIVED 但 label 空 → 剔除
    in[2].play_id = 1; in[2].offset = 0x2A0000; in[2].size = 0x1000;
    in[2].state = 2; in[2].label[0] = '\0';
    // [3] 合法 ARCHIVED
    in[3].play_id = 1; in[3].offset = 0x2B0000; in[3].size = 0x2000;
    in[3].state = 2; strcpy(in[3].label, "content");

    meta_backup_data_t out[META_BACKUP_DATA_MAX];
    int n = meta_backup_filter_import(in, 4, out);
    assert_eq(n, 2, "only valid ARCHIVED records kept");
    assert_str_equal(out[0].label, "save", "kept record 0");
    assert_str_equal(out[1].label, "content", "kept record 1");

    // 空输入 / NULL → 0。
    assert_eq(meta_backup_filter_import(NULL, 4, out), 0, "null input");
    assert_eq(meta_backup_filter_import(in, 0, out), 0, "zero count");
    printf("PASS backup filter import\n");
}

static void test_import_verdict(void)
{
    // 空间足够 → OK。
    assert_eq(meta_backup_import_verdict(0x10000, 0x8000, 0), META_IMPORT_OK,
              "enough free");
    // 不足但有可回收归档 → 先 ARC。
    assert_eq(meta_backup_import_verdict(0x1000, 0x8000, 0x4000),
              META_IMPORT_ERR_NEED_ARC, "reclaimable -> arc");
    // 不足且无可回收 → 507。
    assert_eq(meta_backup_import_verdict(0x1000, 0x8000, 0),
              META_IMPORT_ERR_INSUFFICIENT, "no reclaimable -> insufficient");
    // 无可导入内容 → NO_RECORDS(优先于空间判定)。
    assert_eq(meta_backup_import_verdict(0, 0, 0), META_IMPORT_ERR_NO_RECORDS,
              "nothing to import");
    printf("PASS backup import verdict\n");
}

static void test_size_consistency(void)
{
    assert_true(sizeof(meta_backup_header_t) > 0, "header size > 0");
    assert_true(sizeof(meta_backup_data_t) > 0, "data record size > 0");
    assert_true(META_BACKUP_DATA_MAX >= 8, "DATA_MAX reasonable");
}

int main(void)
{
    test_header_valid();
    test_header_null();
    test_header_invalid_magic();
    test_header_invalid_version();
    test_header_empty_play_id();
    test_header_empty_version();
    test_header_too_many_records();
    test_firmware_match_exact();
    test_firmware_match_mismatch();
    test_serialize_basic();
    test_serialize_no_archived();
    test_parse_roundtrip();
    test_parse_truncated();
    test_parse_bad_magic();
    test_filter_import();
    test_import_verdict();
    test_size_consistency();

    printf("PASS test_meta_backup\n");
    return 0;
}
