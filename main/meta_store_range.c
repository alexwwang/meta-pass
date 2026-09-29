// main/meta_store_range.c —— 契约见头文件。纯逻辑,host 测试链接同一份代码。
#include "meta_store_range.h"

#include <stddef.h>

static bool parse_u32(const char **p, uint32_t *out)
{
    const char *s = *p;
    if (*s < '0' || *s > '9') return false;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        const uint32_t digit = (uint32_t)(*s - '0');
        if (v > (UINT32_MAX - digit) / 10u) return false;
        v = v * 10u + digit;
        s++;
    }
    *p = s;
    *out = v;
    return true;
}

bool meta_store_range_parse(const char *value, meta_store_range_t *out)
{
    if (!value || !out) return false;
    const char *p = value;
    const char prefix[] = "bytes ";
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        if (p[i] != prefix[i]) return false;
    }
    p += sizeof(prefix) - 1;
    if (!parse_u32(&p, &out->first)) return false;
    if (*p++ != '-') return false;
    if (!parse_u32(&p, &out->last)) return false;
    if (*p++ != '/') return false;
    if (!parse_u32(&p, &out->total)) return false;
    if (*p != '\0') return false;
    return out->total > 0 && out->first <= out->last && out->last < out->total;
}

bool meta_store_range_matches(const meta_store_range_t *range,
                              uint32_t expected_first,
                              uint32_t expected_total,
                              int64_t content_length)
{
    if (!range || content_length <= 0) return false;
    const uint32_t span = range->last - range->first + 1u;
    return range->first == expected_first
        && range->total == expected_total
        && (int64_t)span == content_length;
}
