// main/meta_store_prov.c —— 实现见头文件注释。零 IDF 依赖:host 单测
// (tests/test_meta_store_prov.c)与真机(meta_store_net.c)链接同一份代码。
#include "meta_store_prov.h"

#include <stdio.h>
#include <string.h>

// ---- URL 解码(query form 语义:%XX 十六进制;'+')->空格) ----

static int url_hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char *s)
{
    size_t w = 0;
    for (size_t r = 0; s[r] != '\0';) {
        if (s[r] == '%') {
            const int hi = url_hex_nib(s[r + 1]);
            const int lo = (hi >= 0) ? url_hex_nib(s[r + 2]) : -1;
            if (hi >= 0 && lo >= 0) {
                s[w++] = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        if (s[r] == '+') {
            s[w++] = ' ';
            r++;
            continue;
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

// 有界拷贝:返回写入字节数(不含 NUL);src 超过 dst_cap-1 返回 -1(调用方按
// 契约超限拒绝,不静默截断)。
static int copy_capped_chk(char *dst, size_t dst_cap, const char *src)
{
    const size_t n = strlen(src);
    if (n >= dst_cap) return -1;
    memcpy(dst, src, n + 1);
    return (int)n;
}

// ---- 表单解析 ----

bool meta_store_prov_parse_wifi_query(const char *query,
                                      char *ssid, size_t ssid_cap,
                                      char *pass, size_t pass_cap)
{
    if (!query || !ssid || ssid_cap == 0 || !pass || pass_cap == 0) return false;

    // 按 '&' 切键值对,再按首个 '=' 切键/值;值的 '%' 与 '+' 就地解码。
    // 现场容量:最坏 ssid 32B→编码 96、pass 64B→编码 192,头肩 32B 足够。
    char work[340];
    const size_t qlen = strlen(query);
    if (qlen >= sizeof(work)) return false;
    memcpy(work, query, qlen + 1);

    char raw_ssid[33 * 3] = {0};
    char raw_pass[65 * 3] = {0};
    bool got_ssid = false, got_pass = false;

    char *save = NULL;
    for (char *pair = work;; pair = NULL) {
        char *kv = strtok_r(pair, "&", &save);
        if (!kv) break;
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = kv;
        char *val = eq + 1;
        if (strcmp(key, "ssid") == 0 && !got_ssid) {
            if (copy_capped_chk(raw_ssid, sizeof(raw_ssid), val) < 0) return false;
            got_ssid = true;
        } else if (strcmp(key, "ssid") == 0) {
            return false;   // 重复键:契约外,拒绝(不部分信任)
        } else if (strcmp(key, "pass") == 0 && !got_pass) {
            if (copy_capped_chk(raw_pass, sizeof(raw_pass), val) < 0) return false;
            got_pass = true;
        } else if (strcmp(key, "pass") == 0) {
            return false;
        }
    }
    if (!got_ssid || !got_pass) return false;

    url_decode_inplace(raw_ssid);
    url_decode_inplace(raw_pass);
    if (raw_ssid[0] == '\0') return false;   // 与原实现同口径:空 ssid 拒收
    if (raw_pass[0] == '\0') return false;   // 开放网络不支持(页面同口径)

    if (copy_capped_chk(ssid, ssid_cap, raw_ssid) < 0) return false;
    if (copy_capped_chk(pass, pass_cap, raw_pass) < 0) return false;
    return true;
}

// ---- 断连原因码文案 ----

const char *meta_store_wifi_fail_text(int reason, char *buf, size_t cap)
{
    switch (reason) {
    case 15:  return "Wrong password?";              // 4WAY_HANDSHAKE_TIMEOUT
    case 201: return "AP not found. Re-scan.";       // NO_AP_FOUND
    case 202: return "Auth failed: password/PMF?";   // AUTH_FAIL
    case 204: return "Handshake timeout.";           // HANDSHAKE_TIMEOUT
    case 205: return "Router refused (PMF/RSN).";    // CONNECTION_FAIL
    default:  break;
    }
    if (reason == 0) return "WiFi connect failed.";
    if (!buf || cap == 0) return "";
    snprintf(buf, cap, "WiFi failed (code %d).", reason);
    return buf;
}

// ---- SSID JSON 转义 ----

void meta_store_prov_json_escaped_ssid(const uint8_t *ssid, char *out, size_t out_sz)
{
    size_t w = 0;
    if (!out || out_sz == 0) return;
    if (!ssid) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < 32 && ssid[i] != 0; i++) {
        char one[8];
        const uint8_t ch = ssid[i];
        if (ch == '"' || ch == '\\') {
            one[0] = '\\'; one[1] = (char)ch; one[2] = '\0';
        } else if (ch < 0x20) {
            snprintf(one, sizeof(one), "\\u%04x", ch);
        } else {
            one[0] = (char)ch; one[1] = '\0';
        }
        const size_t n = strlen(one);
        if (w + n >= out_sz) break;   // 上界 32×6+1 ≤ 97,正常不触达
        memcpy(out + w, one, n + 1);
        w += n;
    }
    out[w] = '\0';
}

// ---- DNS 门户报文 ----

size_t meta_store_dns_query_end(const uint8_t *pkt, size_t len)
{
    if (len < 17) return 0;                    // 12B 头 + 最短 QNAME(1) + QTYPE(2) + QCLASS(2)
    if ((pkt[2] & 0x80) != 0) return 0;        // QR=1:不是查询
    if (pkt[4] != 0 || pkt[5] != 1) return 0;  // QDCOUNT 必须为 1
    size_t off = 12;
    while (off < len) {                        // QNAME:label 序列
        const uint8_t l = pkt[off];
        if (l == 0) return (off + 1 + 4 <= len) ? off + 1 : 0;
        off += 1u + l;
    }
    return 0;
}
