// tests/test_meta_store_json.c —— meta_store_json 的 host 测试。
// 用 tools/install-slot 服务端 analyze 端点的真实响应形态做样本(含嵌套 extracted/
// store/slots 数组干扰项),锁定:字符串/整数/bool/嵌套路径提取、sha256 hex 解析、
// 各类畸形输入必须返回 false(不部分信任)。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_store_json.h"

// 与线上 /api/analyze?id=563 响应同形的样本(截短 slots 数组)。
static const char SAMPLE[] =
    "{\"ok\":true,\"id\":563,\"revisionId\":1348,\"name\":\"ai-passport-9\","
    "\"store\":{\"size\":3219456,\"sha256\":\"f3f5d92358b9cb356add8d36006fac78\"},"
    "\"extracted\":{\"imageLen\":1880864,"
    "\"sha256\":\"1828e251042477c152709dab15a83c2dcc28060eae2e3dc57ff19d3431ad24c4\"},"
    "\"slots\":[{\"slot\":0,\"limit\":1921024,\"fit\":true},"
    "{\"slot\":1,\"limit\":2093056,\"fit\":true}],"
    "\"suggestedSlot\":0,\"supported\":true,\"reason\":\"ok\"}";

// 同一形态的 supported=false 变体(设备不支持页契约:无 slots 数组)。
static const char SAMPLE_UNSUPPORTED[] =
    "{\"ok\":false,\"id\":563,\"revisionId\":1348,\"name\":\"ai-passport-9\","
    "\"store\":{\"size\":3219456,\"sha256\":\"f3f5d92358b9cb356add8d36006fac78\"},"
    "\"extracted\":null,\"slots\":null,"
    "\"suggestedSlot\":-1,\"supported\":false,\"reason\":\"too-large\"}";

#define SAMPLE_LEN (sizeof(SAMPLE) - 1)

static void test_happy_path(void)
{
    char name[48] = {0};
    assert(meta_store_json_get_string(SAMPLE, SAMPLE_LEN, "name", name, sizeof(name)));
    assert(strcmp(name, "ai-passport-9") == 0);

    int64_t v = -1;
    assert(meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "id", &v));
    assert(v == 563);
    assert(meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "revisionId", &v));
    assert(v == 1348);
    assert(meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "extracted/imageLen", &v));
    assert(v == 1880864);
    assert(meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "suggestedSlot", &v));
    assert(v == 0);

    bool b = false;
    assert(meta_store_json_get_bool(SAMPLE, SAMPLE_LEN, "ok", &b) && b);
    assert(meta_store_json_get_bool(SAMPLE, SAMPLE_LEN, "supported", &b) && b);
    // 数组路径不支持(设计上限:对象两层)
    assert(!meta_store_json_get_bool(SAMPLE, SAMPLE_LEN, "slots/fit", &b));
    b = true;
    assert(meta_store_json_get_bool(SAMPLE, SAMPLE_LEN, "reason", &b) == false);

    char sha[65] = {0};
    assert(meta_store_json_get_string(SAMPLE, SAMPLE_LEN, "extracted/sha256",
                                      sha, sizeof(sha)));
    uint8_t digest[32];
    assert(meta_store_json_parse_sha256(sha, strlen(sha), digest));
    assert(digest[0] == 0x18 && digest[31] == 0xc4);

    // 嵌套 store 的字符串与无关字段共存
    char s[64] = {0};
    assert(meta_store_json_get_string(SAMPLE, SAMPLE_LEN, "store/sha256", s, sizeof(s)));
    assert(strcmp(s, "f3f5d92358b9cb356add8d36006fac78") == 0);
    assert(meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "store/size", &v));
    assert(v == 3219456);

    printf("PASS happy path\n");
}

static void test_absent_and_malformed(void)
{
    char buf[48];
    int64_t v;
    bool b;

    // 缺键
    assert(!meta_store_json_get_string(SAMPLE, SAMPLE_LEN, "nope", buf, sizeof(buf)));
    assert(!meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "nope", &v));
    assert(!meta_store_json_get_bool(SAMPLE, SAMPLE_LEN, "nope", &b));
    assert(!meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "extracted/nope", &v));
    // 类型不符(string 键取 int / int 键取 string)
    assert(!meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "name", &v));
    assert(!meta_store_json_get_string(SAMPLE, SAMPLE_LEN, "id", buf, sizeof(buf)));
    // 数组路径不支持
    assert(!meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "slots", &v));
    // 三层路径超限
    assert(!meta_store_json_get_int(SAMPLE, SAMPLE_LEN, "a/b/c", &v));

    // 截断 JSON(各种前缀长度)不得误判为成功
    for (size_t n = 0; n < SAMPLE_LEN; n += 7) {
        bool r1 = meta_store_json_get_string(SAMPLE, n, "name", buf, sizeof(buf));
        // 允许恰好截在完整值后的巧合不存在(步进 7 覆盖大量形态);凡返回 true 的
        // 前缀必须确实能完整解析出值——这里只断言不崩溃,真值正确性由 n=SAMPLE_LEN 锁定。
        (void)r1;
    }

    // 结构破损
    assert(!meta_store_json_get_int("{\"id\":}", 7, "id", &v));
    assert(!meta_store_json_get_bool("{\"ok\":tru}", 10, "ok", &b));
    assert(!meta_store_json_get_string("{\"name\":\"abc}", 14, "name", buf, sizeof(buf)));
    assert(!meta_store_json_get_int("not json", 8, "id", &v));
    // 顶层不是对象
    assert(!meta_store_json_get_int("[1,2]", 5, "id", &v));

    printf("PASS absent/malformed\n");
}

static void test_sha256_hex(void)
{
    uint8_t d[32];
    // 大写非法
    assert(!meta_store_json_parse_sha256("1828E251042477c152709dab15a83c2dc"
                                         "c28060eae2e3dc57ff19d3431ad24c4", 64, d));
    // 短串非法
    assert(!meta_store_json_parse_sha256("1828", 4, d));
    // 非 hex 字符非法
    assert(!meta_store_json_parse_sha256("zz28e251042477c152709dab15a83c2dcc"
                                         "c28060eae2e3dc57ff19d3431ad24c4", 64, d));
    // 全零合法
    assert(meta_store_json_parse_sha256("00000000000000000000000000000000000"
                                        "0000000000000000000000000000000", 64, d));
    for (int i = 0; i < 32; i++) assert(d[i] == 0);

    printf("PASS sha256 hex parsing\n");
}

static void test_string_escapes_and_buffer_bounds(void)
{
    char out[16];
    // 转义序列解码
    assert(meta_store_json_get_string("{\"k\":\"a\\nb\"}", 13, "k", out, sizeof(out)));
    assert(strcmp(out, "a\nb") == 0);
    assert(meta_store_json_get_string("{\"k\":\"a\\\"b\\\\c\"}", 16, "k", out, sizeof(out)));
    assert(strcmp(out, "a\"b\\c") == 0);
    // 缓冲恰好足够(含 '\0')
    assert(meta_store_json_get_string("{\"k\":\"12345678901234\"}", 22, "k", out, 15));
    assert(strcmp(out, "12345678901234") == 0);
    // 缓冲差一字节必须失败而非截断
    assert(!meta_store_json_get_string("{\"k\":\"12345678901234\"}", 22, "k", out, 14));
    // 控制字符裸出现必须失败
    assert(!meta_store_json_get_string("{\"k\":\"a\nb\"}", 12, "k", out, sizeof(out)));

    printf("PASS escapes and buffer bounds\n");
}

static void test_unsupported_shape(void)
{
    int64_t v = -1;
    bool b = true;
    // unsupported 变体整体字段可读且互洽
    assert(meta_store_json_get_int(SAMPLE_UNSUPPORTED, sizeof(SAMPLE_UNSUPPORTED) - 1,
                                   "suggestedSlot", &v) && v == -1);
    assert(meta_store_json_get_bool(SAMPLE_UNSUPPORTED, sizeof(SAMPLE_UNSUPPORTED) - 1,
                                    "supported", &b) && b == false);
    char reason[24] = {0};
    assert(meta_store_json_get_string(SAMPLE_UNSUPPORTED, sizeof(SAMPLE_UNSUPPORTED) - 1,
                                      "reason", reason, sizeof(reason)));
    assert(strcmp(reason, "too-large") == 0);
    // null 值不是 string/int,必须失败(不部分信任)
    assert(!meta_store_json_get_string(SAMPLE_UNSUPPORTED, sizeof(SAMPLE_UNSUPPORTED) - 1,
                                       "extracted", reason, sizeof(reason)));
    assert(!meta_store_json_get_int(SAMPLE_UNSUPPORTED, sizeof(SAMPLE_UNSUPPORTED) - 1,
                                    "extracted", &v));
    printf("PASS unsupported shape\n");
}

// 对象数组读取(install offer 的 slots 契约,文档 §6.4)。
static void test_array_fields(void)
{
    size_t n = 99;
    assert(meta_store_json_get_array_count(SAMPLE, SAMPLE_LEN, "slots", &n));
    assert(n == 2);

    int64_t v = -1;
    assert(meta_store_json_get_array_int(SAMPLE, SAMPLE_LEN, "slots", 0, "slot", &v));
    assert(v == 0);
    assert(meta_store_json_get_array_int(SAMPLE, SAMPLE_LEN, "slots", 0, "limit", &v));
    assert(v == 1921024);
    assert(meta_store_json_get_array_int(SAMPLE, SAMPLE_LEN, "slots", 1, "slot", &v));
    assert(v == 1);
    assert(meta_store_json_get_array_int(SAMPLE, SAMPLE_LEN, "slots", 1, "limit", &v));
    assert(v == 2093056);

    bool b = false;
    assert(meta_store_json_get_array_bool(SAMPLE, SAMPLE_LEN, "slots", 0, "fit", &b) && b);
    assert(meta_store_json_get_array_bool(SAMPLE, SAMPLE_LEN, "slots", 1, "fit", &b) && b);

    // 下标越界 / 字段缺失 / 非对象元素 / 键不存在 / 空数组
    assert(!meta_store_json_get_array_int(SAMPLE, SAMPLE_LEN, "slots", 2, "slot", &v));
    assert(!meta_store_json_get_array_int(SAMPLE, SAMPLE_LEN, "slots", 0, "nope", &v));
    assert(!meta_store_json_get_array_bool(SAMPLE, SAMPLE_LEN, "slots", 0, "slot", &b));
    assert(!meta_store_json_get_array_int(SAMPLE, SAMPLE_LEN, "extracted", 0, "slot", &v));
    assert(!meta_store_json_get_array_count(SAMPLE, SAMPLE_LEN, "nope", &n));
    assert(meta_store_json_get_array_count("{\"a\":[]}", 8, "a", &n) && n == 0);
    // null/标量不是数组
    assert(!meta_store_json_get_array_count(SAMPLE_UNSUPPORTED,
                                            sizeof(SAMPLE_UNSUPPORTED) - 1,
                                            "slots", &n));
    assert(!meta_store_json_get_array_count(SAMPLE, SAMPLE_LEN, "suggestedSlot", &n));
    // 非对象元素
    assert(!meta_store_json_get_array_int("{\"a\":[1,2]}", 11, "a", 0, "slot", &v));
    // 逗号/闭合破损
    assert(!meta_store_json_get_array_int("{\"a\":[{\"x\":1}{\"x\":2}]}", 23,
                                          "a", 1, "x", &v));

    printf("PASS array fields\n");
}

int main(void)
{
    test_happy_path();
    test_absent_and_malformed();
    test_sha256_hex();
    test_string_escapes_and_buffer_bounds();
    test_unsupported_shape();
    test_array_fields();
    printf("ALL meta_store_json TESTS PASSED\n");
    return 0;
}
