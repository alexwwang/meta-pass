// tests/test_store_analyze_contract.c —— 设备端 /api/analyze 响应解析的合同测试。
// 样本来自本地 server.mjs(store-analyze.js)的真实响应形态。r5 版本曾因把
// name/extracted 当必填,把服务端错误规范形态(name:null/extracted:null)整包
// 判成 format —— 真实原因码(too-large/wrong-chip/…)到不了屏。本文件锁定:
//   1. 本地服务端对 563/675 的真实 supported=true 响应必须逐字段解析成功;
//   2. 不可装/错误响应(name:null/extracted:null、sha256:null)必须保留服务端
//      reason 码;
//   3. 契约漂移(未知 reason、supported 与 reason 矛盾、可装时缺字段、坏 sha、
//      槽位越界)必须拒收(不部分信任)。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_store_analysis.h"

// 本地 server.mjs /api/analyze?id=563 的真实形态(store-analyze.js analyzeJson;
// r6 前后字段一致,sha256 为 64 位小写 hex)。数值取自 2026-09 真实抓包量级。
static const char SAMPLE_OK[] =
    "{\"ok\":true,\"id\":563,\"revisionId\":1348,\"name\":\"ai-passport-9\","
    "\"store\":{\"size\":3219456,\"sha256\":\"f3f5d92358b9cb356add8d36006fac78\"},"
    "\"extracted\":{\"imageLen\":1880864,"
    "\"sha256\":\"1828e251042477c152709dab15a83c2dcc28060eae2e3dc57ff19d3431ad24c4\"},"
    "\"slots\":[{\"slot\":0,\"limit\":1921024,\"fit\":true},"
    "{\"slot\":1,\"limit\":2093056,\"fit\":true}],"
    "\"suggestedSlot\":0,\"supported\":true,\"reason\":\"ok\"}";

// 玩法 675(口袋里的 AI 办公室):rec 分区 subtype 0x40 → 警告放行,r8 真实形态。
static const char SAMPLE_WARN[] =
    "{\"ok\":true,\"id\":675,\"revisionId\":1291,\"name\":\"ai-9\","
    "\"store\":{\"size\":2729792,\"sha256\":\"a1b2c3d4e5f60718293a4b5c6d7e8f90\"},"
    "\"extracted\":{\"imageLen\":2664256,"
    "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"},"
    "\"slots\":[{\"slot\":0,\"limit\":1921024,\"fit\":false},"
    "{\"slot\":2,\"limit\":2740224,\"fit\":true}],"
    "\"suggestedSlot\":2,\"supported\":true,"
    "\"reason\":\"custom-partitions\",\"detail\":\"rec\"}";

// 不可装 · 形态 A(too-large):镜像已取回并解包,但 sha256 惰性为 null,
// name 仍在,suggestedSlot=-1(store-analyze.js analyzeJson 直出)。
static const char SAMPLE_TOO_LARGE[] =
    "{\"ok\":false,\"id\":900,\"revisionId\":77,\"name\":\"big-app\","
    "\"store\":{\"size\":4194304,\"sha256\":\"f3f5d92358b9cb356add8d36006fac78\"},"
    "\"extracted\":{\"imageLen\":4100,\"sha256\":null},"
    "\"slots\":[{\"slot\":0,\"limit\":1921024,\"fit\":false}],"
    "\"suggestedSlot\":-1,\"supported\":false,\"reason\":\"too-large\"}";

// 不可装 · 形态 B(wrong-chip/not-found 等提取失败):错误规范化路径,
// name/store/extracted/slots 全为 null(store-analyze.js got.error 分支)。
static const char SAMPLE_WRONG_CHIP[] =
    "{\"ok\":false,\"id\":901,\"revisionId\":null,\"name\":null,"
    "\"store\":null,\"extracted\":null,\"slots\":null,"
    "\"suggestedSlot\":-1,\"supported\":false,\"reason\":\"wrong-chip\"}";

// 不可装 · 形态 B 带 detail(custom-partitions 硬拒:非 0x40 陌生数据分区)。
// r9:custom-partitions 为单态警告码(supported=true + detail=分区名;
// 形态对应 play 563 的 easter SPIFFS 分区 —— 真实市场回归)。
static const char SAMPLE_WARN_SPIFFS[] =
    "{\"ok\":true,\"id\":902,\"revisionId\":77,\"name\":\"easter-app\","
    "\"store\":{\"size\":100,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"},"
    "\"extracted\":{\"imageLen\":100,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"},"
    "\"slots\":[{\"slot\":0,\"limit\":1921024,\"fit\":true}],"
    "\"suggestedSlot\":0,\"supported\":true,"
    "\"reason\":\"custom-partitions\",\"detail\":\"easter\"}";

static void test_supported_ok(void)
{
    meta_store_analysis_t a;
    assert(meta_store_analysis_parse(SAMPLE_OK, sizeof(SAMPLE_OK) - 1, &a));
    assert(a.supported);
    assert(a.suggested_slot == 0);
    assert(strcmp(a.name, "ai-passport-9") == 0);
    assert(a.image_len == 1880864);
    assert(strcmp(a.reason, "ok") == 0);
    assert(a.detail[0] == '\0');
    // sha256 已从 hex 解成 32 字节(抽查首尾字节)
    assert(a.sha256[0] == 0x18 && a.sha256[31] == 0xc4);
    printf("PASS: supported=true (ok) parses field-by-field\n");
}

static void test_supported_warn_custom_partitions(void)
{
    meta_store_analysis_t a;
    assert(meta_store_analysis_parse(SAMPLE_WARN, sizeof(SAMPLE_WARN) - 1, &a));
    assert(a.supported);
    assert(a.suggested_slot == 2);
    assert(strcmp(a.name, "ai-9") == 0);
    assert(a.image_len == 2664256);
    assert(strcmp(a.reason, "custom-partitions") == 0);
    assert(strcmp(a.detail, "rec") == 0);
    printf("PASS: warn-and-allow (custom-partitions + detail=rec) parses\n");
}

static void test_unsupported_keeps_reason(void)
{
    meta_store_analysis_t a;
    // 形态 A:sha256:null / name 在 —— reason 必须保留,不得判成 format。
    assert(meta_store_analysis_parse(SAMPLE_TOO_LARGE,
                                     sizeof(SAMPLE_TOO_LARGE) - 1, &a));
    assert(!a.supported);
    assert(a.suggested_slot == -1);
    assert(strcmp(a.reason, "too-large") == 0);
    assert(a.image_len == 0 && a.sha256[0] == 0);   // 不部分信任:安装字段不填

    // 形态 B:全 null。
    assert(meta_store_analysis_parse(SAMPLE_WRONG_CHIP,
                                     sizeof(SAMPLE_WRONG_CHIP) - 1, &a));
    assert(!a.supported);
    assert(strcmp(a.reason, "wrong-chip") == 0);

    // r9:警告码恒 supported=true,detail 透传分区名(设备 NOTE 行)。
    assert(meta_store_analysis_parse(SAMPLE_WARN_SPIFFS,
                                     sizeof(SAMPLE_WARN_SPIFFS) - 1, &a));
    assert(a.supported);
    assert(strcmp(a.reason, "custom-partitions") == 0);
    assert(strcmp(a.detail, "easter") == 0);
    assert(strcmp(a.name, "easter-app") == 0 && a.suggested_slot == 0);
    // 反向:custom-partitions + supported=false = 契约漂移,拒收。
    assert(!meta_store_analysis_parse(
        "{\"supported\":false,\"suggestedSlot\":-1,\"reason\":\"custom-partitions\"}",
        66, &a));
    printf("PASS: unsupported shapes keep server reason codes\n");
}

static void test_drift_rejected(void)
{
    meta_store_analysis_t a;

    // 未知 reason 码(服务端契约漂移)。
    assert(!meta_store_analysis_parse(
        "{\"supported\":false,\"suggestedSlot\":-1,\"reason\":\"who-knows\"}",
        62, &a));
    // supported 与 reason 矛盾(ok 必须 supported=true)。
    assert(!meta_store_analysis_parse(
        "{\"supported\":false,\"suggestedSlot\":0,\"reason\":\"ok\"}", 52, &a));
    // too-large 却 supported=true(非双态码不允许反向)。
    assert(!meta_store_analysis_parse(
        "{\"supported\":true,\"suggestedSlot\":-1,\"reason\":\"too-large\"}",
        61, &a));
    // 可装但缺 name。
    assert(!meta_store_analysis_parse(
        "{\"supported\":true,\"suggestedSlot\":0,\"reason\":\"ok\","
        "\"extracted\":{\"imageLen\":100,\"sha256\":"
        "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}",
        152, &a));
    // 可装但 sha256 非 hex。
    assert(!meta_store_analysis_parse(
        "{\"supported\":true,\"suggestedSlot\":0,\"reason\":\"ok\","
        "\"name\":\"x\",\"extracted\":{\"imageLen\":100,\"sha256\":\"zz\"}}",
        106, &a));
    // suggestedSlot 越界(3 槽位机:合法 -1..2)。
    assert(!meta_store_analysis_parse(
        "{\"supported\":false,\"suggestedSlot\":5,\"reason\":\"too-large\"}",
        60, &a));
    // 缺 supported 字段。
    assert(!meta_store_analysis_parse(
        "{\"suggestedSlot\":0,\"reason\":\"ok\"}", 31, &a));
    // 可装但 name 超 MNAM 上限(32 字节)。
    assert(!meta_store_analysis_parse(
        "{\"supported\":true,\"suggestedSlot\":0,\"reason\":\"ok\","
        "\"name\":\"0123456789abcdef0123456789abcdef0\","
        "\"extracted\":{\"imageLen\":100,\"sha256\":"
        "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}",
        186, &a));
    printf("PASS: contract drift rejected without partial trust\n");
}

int main(void)
{
    test_supported_ok();
    test_supported_warn_custom_partitions();
    test_unsupported_keeps_reason();
    test_drift_rejected();
    printf("ALL store-analyze CONTRACT TESTS PASSED\n");
    return 0;
}
