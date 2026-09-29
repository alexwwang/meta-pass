// main/meta_store_range.h —— HTTP Content-Range 响应头解析(纯逻辑,零 ESP-IDF 依赖)。
// 设备端续传只接受 `bytes <first>-<last>/<total>` 单区间;其他形态一律拒绝,
// 不把协议外响应当成可写入 flash 的镜像字节。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t first;
    uint32_t last;
    uint32_t total;
} meta_store_range_t;

// 解析 Content-Range。成功返回 true;语法、越界或 uint32 溢出均返回 false。
bool meta_store_range_parse(const char *value, meta_store_range_t *out);

// 校验解析结果与本次续传契约一致:起点=已收字节、总长=analyze 声明、
// HTTP Content-Length=本次区间字节数。
bool meta_store_range_matches(const meta_store_range_t *range,
                              uint32_t expected_first,
                              uint32_t expected_total,
                              int64_t content_length);
