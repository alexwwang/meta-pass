// main/meta_store_json.h —— 有界、路径感知的极简 JSON 提取器(纯逻辑,host 可测)。
// 只支持商店 analyze 响应契约所需的一个子集:顶层对象 + 一层嵌套对象,
// 值类型 string / number(int) / bool。不分配堆内存、不递归(固定深度 ≤2)、
// 输入不必以 '\0' 结尾(按显式长度遍历)——可直接解析 HTTP 响应体分块拼成的缓冲。
//
// 约定:所有函数遇任何格式偏差(缺键、类型不符、越界)一律返回 false 且不改 out,
// 调用方据此判 "unsupported/format",绝不部分信任残缺 JSON。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 路径形式 "key" 或 "key/subkey"(仅支持至多一层嵌套对象)。key 为逐字节精确匹配,
// 不做转义归一(JSON 键在契约中固定为纯 ASCII 标识符,无转义需求)。
bool meta_store_json_get_string(const char *json, size_t len, const char *path,
                                char *out, size_t out_sz);
bool meta_store_json_get_int(const char *json, size_t len, const char *path,
                             int64_t *out);
bool meta_store_json_get_bool(const char *json, size_t len, const char *path,
                              bool *out);

// SHA-256 的 64 字符小写 hex 串解析为 32 字节(契约中 extracted.sha256 字段)。
// 任何非 [0-9a-f] 字符或长度不足 64 都返回 false。
bool meta_store_json_parse_sha256(const char *hex, size_t len, uint8_t out[32]);
