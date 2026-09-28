// main/meta_store_prov.h —— 配网纯逻辑(零 IDF 依赖,与 meta_store_json 同模式)。
// 从 meta_store_net.c 抽出可 host 测试的部分:表单解析、失败文案、JSON 转义、
// DNS 门户报文。真机构建由 meta_store_net.c 调用;host 单测直接链接本模块
// (tests/test_meta_store_prov.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 解析配网页 POST /api/wifi 的 query("ssid=..&pass=..")。两侧均 URL 解码
// (%XX 与 '+'→空格)。任一失败返回 false(调用方回 400):
//   - ssid/pass 键缺失或值为空;
//   - 解码后 ssid ≥ ssid_cap 或 pass ≥ pass_cap —— 超容量**拒绝**而非静默截断
//     (截断会把 40 字符 SSID 悄悄变成连不上的另一个名字;页面收到 400 显示
//     failed,用户改输,比 NO_AP_FOUND 死等可诊断)。
// cap 含 NUL:常用调用 ssid_cap=33 / pass_cap=65(即最多 32/64 字节,与
// wifi_config_t 字段一致)。
bool meta_store_prov_parse_wifi_query(const char *query,
                                      char *ssid, size_t ssid_cap,
                                      char *pass, size_t pass_cap);

// STA 断连原因码 → 用户可行动的英文短句(IDF v5.x esp_wifi_types.h 常见码)。
// 已知码返回静态字面量(不写 buf);未知的格式化进 buf(可为 NULL,返回空串)。
const char *meta_store_wifi_fail_text(int reason, char *buf, size_t cap);

// SSID 原始字节(≤32,可能无 NUL)→ JSON 字符串体:'"'/ '\\' 转义、控制字符
// \u00XX、高位字节原样输出(页面按 UTF-8 渲染)。输出恒 NUL 结尾,超 cap 截停。
void meta_store_prov_json_escaped_ssid(const uint8_t *ssid, char *out, size_t out_sz);

// Captive-portal DNS 劫持的报文合法性检查:返回 question section 结束偏移
// (可整体回发同一包),不合法返回 0。要求:标准 12B 头、QR=0(查询)、
// QDCOUNT=1、QNAME 标签链完整。
size_t meta_store_dns_query_end(const uint8_t *pkt, size_t len);

// P0 网络页的"改网意图"检测(r10.1):双击 UP 才算用户要改 WiFi —— 单击/
// 误按/其它键一律不干预 ONLINE 自动进 P1(旧版"任意键取消"让误按把用户
// 困在 P0)。窗口 600ms;间隔 ≤ 窗口的两击 = 触发(第二击返回 true);
// 触发后检测器进入待复位态(调用方处理意图后 reset,交接语义明确);
// 时间倒退视为新序列第一击(对调用方时钟源鲁棒)。纯逻辑,host 可测。
#define MPD_PROV_UPCLICK_MS 600

typedef struct {
    int64_t last_ms;   // 上一击时刻;INT64_MIN = 无待击
    bool    fired;     // 已触发未复位
} meta_prov_upclick_t;

void meta_prov_upclick_reset(meta_prov_upclick_t *d);
bool meta_prov_upclick_feed(meta_prov_upclick_t *d, int64_t now_ms);
