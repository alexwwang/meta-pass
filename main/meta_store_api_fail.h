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

// r10.7:OPEN 阶段失败的底层死因(esp_http_client 事件回调捕获)。0=未知。
typedef enum {
    MSAF_CAUSE_NONE = 0,
    MSAF_CAUSE_DNS,          // 域名解析失败(ESP_ERR_ESP_TLS_FAILED_RESOLVE_HOST 等)
    MSAF_CAUSE_TIMEOUT,      // TCP/TLS 连接超时(ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT)
    MSAF_CAUSE_REFUSED,      // 连接被拒/不可达
    MSAF_CAUSE_CERT,         // 证书校验失败(MBEDTLS_ERR_X509* / MBEDTLS_ERR_SSL*)
} msaf_cause_t;

// 设置/清除最近一次 OPEN 死因(设备侧由事件回调调用;线程约束:仅在
// 网络任务上下文的 httpclient 事件内发生,无需锁)。
void meta_store_api_fail_set_cause(msaf_cause_t cause);
msaf_cause_t meta_store_api_fail_get_cause(void);

// r10.7:类型化死因缺失时的原始码上屏(如 "esp=0x8001"/"sys=113"/"tls=-0x2700")。
// 设备侧在事件回调里记录首个非零码;屏显兜底句时附上,真机不接串口也有证据。
void meta_store_api_fail_set_raw(const char *code_str);

// OPEN 失败短句(阶段+时钟+死因三合一):
//   死因已知 → 具体层("DNS failed.\nCheck WiFi/router." 等);
//   死因未知但有原始码 → "TLS failed [<raw>]"(时钟提示让位给真实码);
//   全部未知 → 回落 fail_text 的阶段句(含时钟分支,行为与旧版一致)。
const char *meta_store_api_fail_open_text(bool clock_unsynced,
                                           char *buf, size_t cap);
