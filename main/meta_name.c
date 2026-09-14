// main/meta_name.c —— 实现见头文件注释。

#include "meta_name.h"

#include <string.h>

// 只接受可打印 ASCII:0x20..0x7E。排除控制字符、DEL、非 ASCII。
static bool name_byte_ok(char c)
{
    const uint8_t b = (uint8_t)c;
    return b >= 0x20u && b <= 0x7Eu;
}

// 对 src 做 XOR 折叠:累加每个字节的异或。
static uint8_t xor_fold(const uint8_t *p, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) {
        x ^= p[i];
    }
    return x;
}

size_t meta_name_pack(const char *name, uint8_t *out, size_t cap)
{
    if (!name || out == NULL) return 0;
    const size_t len = strlen(name);
    if (len == 0 || len > META_NAME_MAX) return 0;
    if (cap < META_NAME_BLOB_HEADER + len) return 0;
    // 逐字节检查可打印性;遇到非法字符即拒绝,保持 out 不变。
    for (size_t i = 0; i < len; i++) {
        if (!name_byte_ok(name[i])) return 0;
    }
    // magic "MNAM"
    out[0] = (uint8_t)'M';
    out[1] = (uint8_t)'N';
    out[2] = (uint8_t)'A';
    out[3] = (uint8_t)'M';
    // len
    out[4] = (uint8_t)len;
    // name bytes
    memcpy(out + 5, name, len);
    // checksum: XOR 折叠 [4 .. 5+len)
    out[5 + len] = xor_fold(out + 4, 1 + len);
    return META_NAME_BLOB_HEADER + len;
}

bool meta_name_unpack(const uint8_t *buf, size_t len, char *out, size_t out_cap)
{
    if (!buf || !out) return false;
    if (out_cap < META_NAME_MAX + 1) return false;
    if (len < META_NAME_BLOB_HEADER) {
        out[0] = '\0';
        return false;
    }
    // 校验 magic
    if (buf[0] != 'M' || buf[1] != 'N' || buf[2] != 'A' || buf[3] != 'M') {
        out[0] = '\0';
        return false;
    }
    const uint8_t got_len = (uint8_t)buf[4];
    if (got_len == 0 || got_len > META_NAME_MAX) {
        out[0] = '\0';
        return false;
    }
    if (len < META_NAME_BLOB_HEADER + got_len) {
        out[0] = '\0';
        return false;
    }
    // 校验每个 name 字节可打印
    for (uint8_t i = 0; i < got_len; i++) {
        if (!name_byte_ok((char)buf[5 + i])) {
            out[0] = '\0';
            return false;
        }
    }
    // 校验 XOR checksum
    const uint8_t expected = xor_fold(buf + 4, 1 + got_len);
    if (buf[5 + got_len] != expected) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, buf + 5, got_len);
    out[got_len] = '\0';
    return true;
}

size_t meta_name_pack_tail(const char *name, uint8_t *window, size_t cap)
{
    if (!window || cap < META_NAME_BLOB_RESERVE) return 0;
    uint8_t blob[META_NAME_BLOB_MAX];
    const size_t blob_len = meta_name_pack(name, blob, sizeof(blob));
    if (blob_len == 0) return 0;
    const size_t start = META_NAME_BLOB_RESERVE - blob_len;
    memcpy(window + start, blob, blob_len);
    return blob_len;
}

bool meta_name_unpack_tail(const uint8_t *window, size_t len, char *out, size_t out_cap)
{
    if (!window || !out) return false;
    if (out_cap < META_NAME_MAX + 1) return false;
    if (len < META_NAME_BLOB_RESERVE) {
        out[0] = '\0';
        return false;
    }

    // 右对齐:blob_len = 6 + name_len,start = 40 - blob_len,checksum 必须落在窗口末尾。
    const size_t min_start = META_NAME_BLOB_RESERVE - META_NAME_BLOB_MAX;       // 2
    const size_t max_start = META_NAME_BLOB_RESERVE - (META_NAME_BLOB_HEADER + 1); // 33
    for (size_t start = min_start; start <= max_start; start++) {
        const uint8_t got_len = window[start + 4];
        if (got_len == 0 || got_len > META_NAME_MAX) continue;
        if (start + META_NAME_BLOB_HEADER + got_len != META_NAME_BLOB_RESERVE) continue;
        if (meta_name_unpack(window + start, META_NAME_BLOB_RESERVE - start, out, out_cap)) {
            return true;
        }
    }
    out[0] = '\0';
    return false;
}
