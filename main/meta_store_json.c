// main/meta_store_json.c —— 实现见头文件注释。
// 解析器为手写扫描:skip_ws → value;value 按首字符分派;string 支持 \" \\ \/ \b \f
// \n \r \t 与 \uXXXX(后随代理对不特殊处理——契约键/值均为 BMP 内 ASCII);
// object/array 跳过用深度计数,不建结构。全部操作带显式剩余长度,杜绝越界读。
#include "meta_store_json.h"

#include <string.h>
#include <stdio.h>

// ---- 游标 ----

typedef struct {
    const char *p;
    size_t len;
    size_t pos;   // 下一个待读字节
} cur_t;

static size_t remain(const cur_t *c) { return c->len - c->pos; }

static char peek(const cur_t *c)
{
    return c->pos < c->len ? c->p[c->pos] : '\0';
}

static bool take(cur_t *c, char ch)
{
    if (peek(c) == ch) {
        c->pos++;
        return true;
    }
    return false;
}

static void skip_ws(cur_t *c)
{
    while (c->pos < c->len) {
        const char ch = c->p[c->pos];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        c->pos++;
    }
}

// ---- 值扫描 ----

// 解析 string(假定当前位置是开引号),把解码后内容拷入 out(含 '\0')。
// 不截断:超出 out_sz 即失败(false),调用方给的缓冲必须够大(契约字段有固定上限)。
static bool scan_string(cur_t *c, char *out, size_t out_sz, size_t *out_len)
{
    if (!take(c, '"')) return false;
    size_t w = 0;
    while (c->pos < c->len) {
        const unsigned char ch = (unsigned char)c->p[c->pos++];
        if (ch == '"') {
            if (out) {
                if (w + 1 > out_sz) return false;
                out[w] = '\0';
            }
            if (out_len) *out_len = w;
            return true;
        }
        if (ch < 0x20u) return false;   // 控制字符不允许出现在 JSON 字符串中
        if (ch == '\\') {
            if (c->pos >= c->len) return false;
            const char esc = c->p[c->pos++];
            char dec;
            switch (esc) {
            case '"':  dec = '"';  break;
            case '\\': dec = '\\'; break;
            case '/':  dec = '/';  break;
            case 'b':  dec = '\b'; break;
            case 'f':  dec = '\f'; break;
            case 'n':  dec = '\n'; break;
            case 'r':  dec = '\r'; break;
            case 't':  dec = '\t'; break;
            case 'u':
                // 契约值均为 ASCII;跳过 4 位 hex 即可,不真正解码。
                if (remain(c) < 4) return false;
                for (int i = 0; i < 4; i++) {
                    const char h = c->p[c->pos + i];
                    const bool hex = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f')
                                  || (h >= 'A' && h <= 'F');
                    if (!hex) return false;
                }
                c->pos += 4;
                // 以 '?' 占位(仅键/短值路径会命中真实解码需求,当前契约无 \u 值)
                dec = '?';
                break;
            default:
                return false;
            }
            if (out) {
                if (w + 1 >= out_sz) return false;
                out[w] = dec;
            }
            w++;
            continue;
        }
        if (out) {
            if (w + 1 >= out_sz) return false;
            out[w] = (char)ch;
        }
        w++;
    }
    return false;   // 未闭合
}

// 跳过任意 value(当前位置是 value 首字符)。string 精确消耗;number/true/false/null
// 按字面量;object/array 深度计数。只用于掠过无关字段。
static bool skip_value(cur_t *c)
{
    skip_ws(c);
    const char ch = peek(c);
    if (ch == '"') {
        return scan_string(c, NULL, 0, NULL);
    }
    if (ch == '{') {
        c->pos++;
        skip_ws(c);
        if (take(c, '}')) return true;
        for (;;) {
            if (!scan_string(c, NULL, 0, NULL)) return false;
            skip_ws(c);
            if (!take(c, ':')) return false;
            if (!skip_value(c)) return false;
            skip_ws(c);
            if (take(c, ',')) {
                skip_ws(c);
                continue;
            }
            return take(c, '}');
        }
    }
    if (ch == '[') {
        c->pos++;
        skip_ws(c);
        if (take(c, ']')) return true;
        for (;;) {
            if (!skip_value(c)) return false;
            skip_ws(c);
            if (take(c, ',')) {
                skip_ws(c);
                continue;
            }
            return take(c, ']');
        }
    }
    if (ch == 't') {
        if (remain(c) >= 4 && memcmp(c->p + c->pos, "true", 4) == 0) {
            c->pos += 4;
            return true;
        }
        return false;
    }
    if (ch == 'f') {
        if (remain(c) >= 5 && memcmp(c->p + c->pos, "false", 5) == 0) {
            c->pos += 5;
            return true;
        }
        return false;
    }
    if (ch == 'n') {
        if (remain(c) >= 4 && memcmp(c->p + c->pos, "null", 4) == 0) {
            c->pos += 4;
            return true;
        }
        return false;
    }
    // number:-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? —— 只校验并消耗。
    {
        size_t start = c->pos;
        take(c, '-');
        if (!take(c, '0')) {
            int digits = 0;
            while (peek(c) >= '0' && peek(c) <= '9') {
                c->pos++;
                digits++;
            }
            if (digits == 0) return false;
        }
        if (take(c, '.')) {
            int digits = 0;
            while (peek(c) >= '0' && peek(c) <= '9') {
                c->pos++;
                digits++;
            }
            if (digits == 0) return false;
        }
        if (peek(c) == 'e' || peek(c) == 'E') {
            c->pos++;
            if (!take(c, '+')) (void)take(c, '-');
            int digits = 0;
            while (peek(c) >= '0' && peek(c) <= '9') {
                c->pos++;
                digits++;
            }
            if (digits == 0) return false;
        }
        return c->pos > start;
    }
}

// ---- 路径查找 ----

// 在 obj 游标(位于 '{' 之后已 skip_ws)内查找 key;命中返回时游标停在 value 首字符
// 之前(已 skip_ws);未命中游标停在 '}' 之后。任何结构偏差返回 false。
static bool obj_find(cur_t *c, const char *key, bool *found)
{
    *found = false;
    skip_ws(c);
    if (take(c, '}')) return true;   // 空对象
    for (;;) {
        char k[24];
        if (!scan_string(c, k, sizeof(k), NULL)) return false;
        skip_ws(c);
        if (!take(c, ':')) return false;
        skip_ws(c);
        const bool hit = strcmp(k, key) == 0;
        if (hit) {
            *found = true;
            return true;             // 游标停在 value 首字符
        }
        if (!skip_value(c)) return false;
        skip_ws(c);
        if (take(c, ',')) {
            skip_ws(c);
            continue;
        }
        return take(c, '}');
    }
}

// path = "key" 或 "key/subkey"。depth 固定最多两层,用两段顺序查找实现,无递归。
static bool locate(const char *json, size_t len, const char *path, cur_t *out_cur)
{
    char seg[2][24];
    int nseg = 0;
    {
        size_t start = 0;
        for (size_t i = 0;; i++) {
            const char ch = path[i];
            if (ch == '/' || ch == '\0') {
                if (nseg >= 2 || i - start == 0 || i - start >= sizeof(seg[0])) return false;
                memcpy(seg[nseg], path + start, i - start);
                seg[nseg][i - start] = '\0';
                nseg++;
                if (ch == '\0') break;
                start = i + 1;
            }
        }
    }

    cur_t c = { .p = json, .len = len, .pos = 0 };
    skip_ws(&c);
    if (!take(&c, '{')) return false;
    for (int depth = 0; depth < nseg; depth++) {
        bool found = false;
        if (!obj_find(&c, seg[depth], &found)) return false;
        if (!found) return false;
        if (depth + 1 < nseg) {
            skip_ws(&c);
            if (!take(&c, '{')) return false;
        }
    }
    *out_cur = c;
    return true;
}

bool meta_store_json_get_string(const char *json, size_t len, const char *path,
                                char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    cur_t c;
    if (!locate(json, len, path, &c)) return false;
    return scan_string(&c, out, out_sz, NULL);
}

// 从游标(位于 value 首字符)解析整数。只允许纯整数(契约 number 字段均为整数;
// 带小数/指数的按不符处理);先用 skip_value 定位 token 边界再逐位复核。
static bool scan_int(cur_t *c, int64_t *out)
{
    const size_t start = c->pos;
    if (!skip_value(c)) return false;
    int64_t v = 0;
    bool neg = false;
    size_t i = start;
    if (i < c->len && c->p[i] == '-') {
        neg = true;
        i++;
    }
    size_t digits = 0;
    for (; i < c->pos; i++) {
        const char ch = c->p[i];
        if (ch < '0' || ch > '9') return false;
        // 先判溢出再乘加:契约数字远小于 int64,超长整数一律拒绝(拒绝点即 false,不截断)
        if (v > ((int64_t)INT64_MAX - 9) / 10) return false;
        v = v * 10 + (ch - '0');
        digits++;
    }
    if (digits == 0) return false;
    *out = neg ? -v : v;
    return true;
}

// 从游标(位于 value 首字符)解析 bool 字面量(true/false,无空白前缀假设已 skip)。
static bool scan_bool(cur_t *c, bool *out)
{
    if (remain(c) >= 4 && memcmp(c->p + c->pos, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (remain(c) >= 5 && memcmp(c->p + c->pos, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool meta_store_json_get_int(const char *json, size_t len, const char *path,
                             int64_t *out)
{
    if (!out) return false;
    cur_t c;
    if (!locate(json, len, path, &c)) return false;
    return scan_int(&c, out);
}

bool meta_store_json_get_bool(const char *json, size_t len, const char *path,
                              bool *out)
{
    if (!out) return false;
    cur_t c;
    if (!locate(json, len, path, &c)) return false;
    return scan_bool(&c, out);
}

// ---- 对象数组读取(install offer slots 契约) ----

// 定位顶层对象中的数组键:返回时游标停在 '[' 之后(已 skip_ws)。
static bool locate_array(const char *json, size_t len, const char *key, cur_t *out)
{
    cur_t c;
    if (!locate(json, len, key, &c)) return false;
    skip_ws(&c);
    if (!take(&c, '[')) return false;
    *out = c;
    return true;
}

// 推进到数组第 idx 个元素(返回时已 skip_ws,停在元素 value 首字符)。
// 元素不存在或结构偏差(缺逗号/提前收尾)返回 false。
static bool array_seek(cur_t *c, size_t idx)
{
    skip_ws(c);
    if (take(c, ']')) return false;   // 空数组:任何下标都不存在
    for (size_t i = 0;; i++) {
        skip_ws(c);
        if (i == idx) return true;
        if (!skip_value(c)) return false;
        skip_ws(c);
        if (take(c, ',')) continue;
        return false;                 // 元素数不足,或缺 ',' 却未闭合
    }
}

// 数组元素对象内字段定位:返回时游标停在 field 的 value 首字符。
static bool array_field(const char *json, size_t len, const char *key, size_t idx,
                        const char *field, cur_t *out)
{
    cur_t c;
    if (!locate_array(json, len, key, &c)) return false;
    if (!array_seek(&c, idx)) return false;
    if (!take(&c, '{')) return false;   // 元素必须是对象
    bool found = false;
    if (!obj_find(&c, field, &found)) return false;
    if (!found) return false;
    *out = c;
    return true;
}

bool meta_store_json_get_array_count(const char *json, size_t len, const char *key,
                                     size_t *out)
{
    if (!out) return false;
    cur_t c;
    if (!locate_array(json, len, key, &c)) return false;
    skip_ws(&c);
    if (take(&c, ']')) { *out = 0; return true; }
    size_t n = 0;
    for (;;) {
        if (!skip_value(&c)) return false;
        n++;
        skip_ws(&c);
        if (take(&c, ',')) continue;
        if (take(&c, ']')) { *out = n; return true; }
        return false;
    }
}

bool meta_store_json_get_array_int(const char *json, size_t len, const char *key,
                                   size_t idx, const char *field, int64_t *out)
{
    if (!out) return false;
    cur_t c;
    if (!array_field(json, len, key, idx, field, &c)) return false;
    return scan_int(&c, out);
}

bool meta_store_json_get_array_bool(const char *json, size_t len, const char *key,
                                    size_t idx, const char *field, bool *out)
{
    if (!out) return false;
    cur_t c;
    if (!array_field(json, len, key, idx, field, &c)) return false;
    return scan_bool(&c, out);
}

bool meta_store_json_get_array_string(const char *json, size_t len, const char *key,
                                      size_t idx, const char *field, char *out, size_t out_sz)
{
    if (!json || len == 0 || !out || out_sz == 0) return false;
    // 定位 key[idx].field: key 必须是顶层数组键,field 在元素对象内。
    char path[256];
    int n = snprintf(path, sizeof(path), "%.128s[%zu].%.64s", key, idx, field);
    if (n < 0 || n >= (int)sizeof(path)) return false;
    return meta_store_json_get_string(json, len, path, out, out_sz);
}

bool meta_store_json_parse_sha256(const char *hex, size_t len, uint8_t out[32])
{
    if (!hex || len < 64 || !out) return false;
    for (int i = 0; i < 32; i++) {
        uint8_t hi = 0, lo = 0;
        const char h = hex[i * 2];
        const char l = hex[i * 2 + 1];
        if (h >= '0' && h <= '9') hi = (uint8_t)(h - '0');
        else if (h >= 'a' && h <= 'f') hi = (uint8_t)(h - 'a' + 10);
        else return false;
        if (l >= '0' && l <= '9') lo = (uint8_t)(l - '0');
        else if (l >= 'a' && l <= 'f') lo = (uint8_t)(l - 'a' + 10);
        else return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
