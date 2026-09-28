// main/meta_store_api_fail.h —— analyze 传输失败分类(纯逻辑,零依赖)。
// 真机与 host 测试(tests/test_meta_store_api_fail.c)链接同一份代码。
//
// 背景(BUG-12):api.c 曾把一切传输失败塌缩成 reason="unavailable" —— TLS
// 握手失败、DNS 失败、读超时、5xx、响应解析失败在屏上一个样,真机上无从
// 定位。本模块按「失败阶段 × 时钟状态」产出可行动的短句;业务 reason 码
// (not-found/format/too-large,服务端契约)不在分类范围,原样透传。
//
// code 编码(单一 int 承载,调用点只多写一个负号):
//   1..3        MSAF_STAGE_* 传输阶段(open/headers/read 失败)
//   -1..-3      MSAF_REASON_* 业务码 → 契约名(not-found/format/too-large)
//   -100..-999  HTTP 状态码取负(-500 = 502/500/503...)→ "Server error N"
//   其它正数    未登记的服务端 reason 序号 → "reason-N"
//   其它负数    兜底 → "Connection lost. Retry."
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 传输失败阶段(api.c 的失败点一一对应;状态码不走阶段,取负直传)。
typedef enum {
    MSAF_STAGE_OPEN = 1,     // esp_http_client_open 失败:TCP/TLS 握手/DNS
    MSAF_STAGE_HEADERS = 2,  // fetch_headers < 0:连接建立但无响应头
    MSAF_STAGE_READ = 3,     // 读体中断/超时
    MSAF_STAGE_PARSE = 4,    // 响应超上限/契约外形态(连接与读取都正常)
} msaf_stage_t;

// 业务 reason 码(服务端契约,负值区间,原样透传上屏)。
enum {
    MSAF_REASON_OK = 0,
    MSAF_REASON_NOT_FOUND = -1,
    MSAF_REASON_FORMAT = -2,
    MSAF_REASON_TOO_LARGE = -3,
};

// 失败 → 用户可行动的英文短句。返回静态字面量或 buf(格式化形态);
// 恒同时写入 buf(cap 允许时,截断 NUL 结尾)。buf 可为 NULL(不写不崩),
// cap=0 不写。
const char *meta_store_api_fail_text(int code, bool clock_unsynced,
                                     char *buf, size_t cap);
