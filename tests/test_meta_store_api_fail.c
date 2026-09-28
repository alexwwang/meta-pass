// tests/test_meta_store_api_fail.c —— analyze/install 传输失败分类器 host 单测。
// 回归背景(BUG-12):设备端所有传输层失败(TLS 握手/DNS/读超时/5xx/解析失败)
// 曾全部塌缩成 reason="unavailable" —— 真机上"证书失败"和"服务器挂了"在屏上
// 一模一样,反复改服务端契约也没用,因为设备自己的失败从来不可见。
// 锁定契约:
//   1. 阶段 × 时钟 → 文案矩阵(时钟未同步 × TLS = 最可能的真机死因,显式上屏);
//   2. esp_http_client 的 http-%d 前缀错误原样透传;
//   3. 业务 reason 码(not-found/format/too-large/...)不受影响;
//   4. 缓冲安全:cap=0 不写不崩,截断恒 NUL 结尾。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_store_api_fail.h"

static void test_matrix(void)
{
    char buf[64];

    // 时钟未同步 × TLS/连接阶段 = 真机最可能的死因,必须显式点名。
    meta_store_api_fail_text(MSAF_STAGE_OPEN, true, buf, sizeof(buf));
    assert(strcmp(buf, "TLS failed (clock unsynced).") == 0);
    meta_store_api_fail_text(MSAF_STAGE_HEADERS, true, buf, sizeof(buf));
    assert(strcmp(buf, "TLS failed (clock unsynced).") == 0);

    // 时钟正常:按阶段给动作建议。
    meta_store_api_fail_text(MSAF_STAGE_OPEN, false, buf, sizeof(buf));
    assert(strcmp(buf, "TLS/DNS failed. Retry.") == 0);
    meta_store_api_fail_text(MSAF_STAGE_HEADERS, false, buf, sizeof(buf));
    assert(strcmp(buf, "No response. Retry.") == 0);
    meta_store_api_fail_text(MSAF_STAGE_READ, false, buf, sizeof(buf));
    assert(strcmp(buf, "Connection lost. Retry.") == 0);

    // 时钟未同步 × 读体/解析阶段:连接已建立,时钟不是死因,不提时钟。
    meta_store_api_fail_text(MSAF_STAGE_READ, true, buf, sizeof(buf));
    assert(strcmp(buf, "Connection lost. Retry.") == 0);
    meta_store_api_fail_text(MSAF_STAGE_PARSE, false, buf, sizeof(buf));
    assert(strcmp(buf, "Bad response from server.") == 0);

    // 服务器错误:带码号(状态码取负编码,见头文件)。
    meta_store_api_fail_text(-500, false, buf, sizeof(buf));
    assert(strcmp(buf, "Server error 500") == 0);
    meta_store_api_fail_text(-503, false, buf, sizeof(buf));
    assert(strcmp(buf, "Server error 503") == 0);
    printf("PASS: stage x clock matrix\n");
}

static void test_http_prefixed(void)
{
    char buf[64];
    // esp_http_client 系列错误带 "http-%d" 前缀:归入 Server error N(码号透传)。
    meta_store_api_fail_text(-230, false, buf, sizeof(buf));   // http-230
    assert(strcmp(buf, "Server error 230") == 0);
    meta_store_api_fail_text(-305, false, buf, sizeof(buf));
    assert(strcmp(buf, "Server error 305") == 0);
    // 兜底:未知负值(非 HTTP 区间)。
    meta_store_api_fail_text(-9999, false, buf, sizeof(buf));
    assert(strcmp(buf, "Connection lost. Retry.") == 0);
    printf("PASS: http-<n> prefixed passthrough\n");
}

static void test_business_reason_passthrough(void)
{
    char buf[64];
    // 业务码与传输码用 sign 区分:reason 码(正数)原样回显,不受分类器影响。
    meta_store_api_fail_text(MSAF_REASON_NOT_FOUND, false, buf, sizeof(buf));
    assert(strcmp(buf, "not-found") == 0);
    meta_store_api_fail_text(MSAF_REASON_FORMAT, false, buf, sizeof(buf));
    assert(strcmp(buf, "format") == 0);
    meta_store_api_fail_text(MSAF_REASON_TOO_LARGE, false, buf, sizeof(buf));
    assert(strcmp(buf, "too-large") == 0);
    // 任意正数(服务端未来新增的码)也原样透传。
    meta_store_api_fail_text(7, false, buf, sizeof(buf));
    assert(strcmp(buf, "reason-7") == 0);
    printf("PASS: business reason codes pass through\n");
}

static void test_buffer_safety(void)
{
    char tiny[8];
    meta_store_api_fail_text(MSAF_STAGE_OPEN, true, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));            // 截断但 NUL 结尾
    meta_store_api_fail_text(MSAF_STAGE_OPEN, false, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));

    // cap=0:不写不崩。
    meta_store_api_fail_text(MSAF_STAGE_OPEN, false, tiny, 0);
    // NULL buf:不崩。
    meta_store_api_fail_text(MSAF_STAGE_OPEN, false, NULL, sizeof(tiny));
    printf("PASS: buffer safety (tiny/zero/NULL)\n");
}

int main(void)
{
    test_matrix();
    test_http_prefixed();
    test_business_reason_passthrough();
    test_buffer_safety();
    printf("ALL META_STORE_API_FAIL TESTS PASSED\n");
    return 0;
}
