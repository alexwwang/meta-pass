// tests/test_meta_store_prov.c —— 配网纯逻辑 host 单测(meta_store_prov.c)。
// 背景:r8 真机配网连环 bug(表单/扫描/原因码)时这一层完全没有测试。
// 锁定的契约:
//   - 表单解析:URL 编码还原、+→空格、空值/缺键/重复键/超容量一律拒绝;
//     超容量必须拒绝而非截断(截断 = 静默改写成连不上的另一个名字);
//   - 原因码文案:15/201/202/204/205 的用户语义(原因码此前从未上屏,
//     r8 第一版还把 201 写错过 —— 本表就是防线);
//   - SSID JSON 转义:引号/反斜杠/控制字符,输出恒 NUL 结尾;
//   - DNS 门户报文:合法查询收尾偏移、响应/多问/短包/标签越界拒绝。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_store_prov.h"

// ---- 表单解析 ----

static void test_parse_wifi_query(void)
{
    char ssid[33], pass[65];

    // 常规路径。
    assert(meta_store_prov_parse_wifi_query("ssid=home&pass=secret99",
                                            ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(strcmp(ssid, "home") == 0 && strcmp(pass, "secret99") == 0);

    // + 解码为空格;%XX 还原(含 %2B → 字面 '+')。
    assert(meta_store_prov_parse_wifi_query(
        "ssid=my+network&pass=p%40ss%2Bword",
        ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(strcmp(ssid, "my network") == 0);
    assert(strcmp(pass, "p@ss+word") == 0);

    // pass 键缺席 / ssid 空 / pass 空 → 拒收(开放网络不支持,页面同口径)。
    assert(!meta_store_prov_parse_wifi_query(
        "ssid=home", ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(!meta_store_prov_parse_wifi_query(
        "ssid=&pass=x", ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(!meta_store_prov_parse_wifi_query(
        "ssid=home&pass=", ssid, sizeof(ssid), pass, sizeof(pass)));

    // 重复键 → 拒收(不部分信任)。
    assert(!meta_store_prov_parse_wifi_query(
        "ssid=a&ssid=b&pass=x", ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(!meta_store_prov_parse_wifi_query(
        "ssid=a&pass=x&pass=y", ssid, sizeof(ssid), pass, sizeof(pass)));

    // 畸形:无 '=' 的段跳过;百分号不完整按字面走。
    assert(meta_store_prov_parse_wifi_query(
        "junk&ssid=h&pass=p", ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(strcmp(ssid, "h") == 0 && strcmp(pass, "p") == 0);
    assert(meta_store_prov_parse_wifi_query(
        "ssid=h&pass=1%ZZ2", ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(strcmp(pass, "1%ZZ2") == 0);

    // 32 字节 SSID(满容量,合法)。
    assert(meta_store_prov_parse_wifi_query(
        "ssid=abcdefghijklmnopqrstuvwxyz012345&pass=p",
        ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(strlen(ssid) == 32);

    // 33 字节 SSID → 解码后超 cap → 拒绝(不截断成另一个名字)。
    assert(!meta_store_prov_parse_wifi_query(
        "ssid=abcdefghijklmnopqrstuvwxyz0123456&pass=p",
        ssid, sizeof(ssid), pass, sizeof(pass)));

    // 64 字节密码(满容量)与 65 字节(超限)。
    const char *p64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char q[256];
    snprintf(q, sizeof(q), "ssid=h&pass=%s", p64);
    assert(meta_store_prov_parse_wifi_query(q, ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(strlen(pass) == 64);
    snprintf(q, sizeof(q), "ssid=h&pass=%sX", p64);
    assert(!meta_store_prov_parse_wifi_query(q, ssid, sizeof(ssid), pass, sizeof(pass)));

    // 调用方容量契约:同一条 query,小缓冲装不下(8 字节装 9 字节 ssid)拒绝、
    // 大缓冲通过。
    char tiny[8];
    assert(!meta_store_prov_parse_wifi_query("ssid=homework&pass=p", tiny, sizeof(tiny),
                                             pass, sizeof(pass)));
    assert(meta_store_prov_parse_wifi_query("ssid=home&pass=p", tiny, sizeof(tiny),
                                            pass, sizeof(pass)));
    assert(strcmp(tiny, "home") == 0);
    printf("PASS: wifi form parsing (decode, rejects, capacity contract)\n");
}

// ---- 原因码文案 ----

static void test_wifi_fail_text(void)
{
    assert(strcmp(meta_store_wifi_fail_text(15, NULL, 0), "Wrong password?") == 0);
    assert(strcmp(meta_store_wifi_fail_text(201, NULL, 0), "AP not found. Re-scan.") == 0);
    assert(strcmp(meta_store_wifi_fail_text(202, NULL, 0), "Auth failed: password/PMF?") == 0);
    assert(strcmp(meta_store_wifi_fail_text(204, NULL, 0), "Handshake timeout.") == 0);
    assert(strcmp(meta_store_wifi_fail_text(205, NULL, 0), "Router refused (PMF/RSN).") == 0);
    assert(strcmp(meta_store_wifi_fail_text(0, NULL, 0), "WiFi connect failed.") == 0);

    // 未知码:格式化进 buf;buf 缺席时空串(不崩)。
    char buf[32];
    assert(strcmp(meta_store_wifi_fail_text(123, buf, sizeof(buf)),
                  "WiFi failed (code 123).") == 0);
    assert(meta_store_wifi_fail_text(123, NULL, 0)[0] == '\0');
    printf("PASS: disconnect reason texts (15/201/202/204/205 + fallback)\n");
}

// ---- SSID JSON 转义 ----

static void test_json_escaped_ssid(void)
{
    char out[97];

    // 契约:入参是 wifi_ap_record_t 的 32 字节 ssid 字段(可无 NUL)——
    // 测试数组一律 32 字节(零填满尾部),与真实调用一致。

    // 引号与反斜杠。
    const uint8_t q[32] = { 'a', '"', 'b', '\\', 'c' };
    meta_store_prov_json_escaped_ssid(q, out, sizeof(out));
    assert(strcmp(out, "a\\\"b\\\\c") == 0);

    // 控制字符 → \u00xx;高位字节原样(UTF-8 语义在页面)。
    const uint8_t ctl[32] = { 0x01, 'x', 0x1f };
    meta_store_prov_json_escaped_ssid(ctl, out, sizeof(out));
    assert(strcmp(out, "\\u0001x\\u001f") == 0);
    const uint8_t hi[32] = { 0xE4, 0xB8, 0xAD };
    meta_store_prov_json_escaped_ssid(hi, out, sizeof(out));
    assert(strcmp(out, "\xe4\xb8\xad") == 0);

    // 32 字节满长无 NUL:恰好收满(超界截停)。
    uint8_t full[34];
    memset(full, 'a', sizeof(full));   // 超过 32 的尾巴必须被忽略
    meta_store_prov_json_escaped_ssid(full, out, sizeof(out));
    assert(strlen(out) == 32);

    // 小输出缓冲:截停且恒 NUL 结尾。
    const uint8_t s3[32] = { 'a', 'b', 'c' };
    char tiny[3] = { 'X', 'X', 'X' };
    meta_store_prov_json_escaped_ssid(s3, tiny, sizeof(tiny));
    assert(tiny[0] == 'a' && tiny[1] == 'b' && tiny[2] == '\0');
    printf("PASS: ssid JSON escaping (quotes, control, bounds, NUL)\n");
}

// ---- DNS 门户报文 ----

static void test_dns_query_end(void)
{
    // 合法查询:头 + QNAME(3 "www" 7 "example" 3 "com" 0) + QTYPE/QCLASS。
    uint8_t pkt[64] = {0};
    pkt[2] = 0x01;            // RD=1(标准手机探测)
    pkt[4] = 0; pkt[5] = 1;   // QDCOUNT=1
    size_t off = 12;
    pkt[off++] = 3; memcpy(pkt + off, "www", 3); off += 3;
    pkt[off++] = 7; memcpy(pkt + off, "example", 7); off += 7;
    pkt[off++] = 3; memcpy(pkt + off, "com", 3); off += 3;
    pkt[off++] = 0;
    off += 4;                 // QTYPE + QCLASS
    // 返回值 = QNAME 收尾偏移(头 + QNAME,即回发长度);QTYPE/QCLASS 的 4 字节
    // 只要求存在(实机已验证的门户行为),不计入返回值。
    assert(meta_store_dns_query_end(pkt, off) == off - 4);
    // QTYPE/QCLASS 缺失(QNAME 后不足 4 字节)→ 拒绝。
    assert(meta_store_dns_query_end(pkt, off - 1) == 0);
    assert(meta_store_dns_query_end(pkt, off - 4) == 0);

    // 响应报文(QR=1)拒绝。
    pkt[2] |= 0x80;
    assert(meta_store_dns_query_end(pkt, off) == 0);
    pkt[2] &= (uint8_t)~0x80;

    // QDCOUNT != 1 拒绝。
    pkt[5] = 2;
    assert(meta_store_dns_query_end(pkt, off) == 0);
    pkt[5] = 1;

    // 短包拒绝(头都不全 / 无 QNAME 结束)。
    assert(meta_store_dns_query_end(pkt, 8) == 0);

    // 标签长度越过包尾拒绝(label 0x7f 但只有 20 字节)。
    uint8_t bad[20] = {0};
    bad[2] = 0x01; bad[5] = 1; bad[12] = 0x7f;
    assert(meta_store_dns_query_end(bad, sizeof(bad)) == 0);
    printf("PASS: dns portal packet validation\n");
}

int main(void)
{
    test_parse_wifi_query();
    test_wifi_fail_text();
    test_json_escaped_ssid();
    test_dns_query_end();
    printf("ALL META_STORE_PROV TESTS PASSED\n");
    return 0;
}
