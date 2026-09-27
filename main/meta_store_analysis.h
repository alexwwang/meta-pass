// main/meta_store_analysis.h —— /api/analyze 响应体解析(纯逻辑,无 IDF 依赖)。
// 独立编译单元:与 meta_store_json.h 同模式,host 合同测试直接链接
// main/meta_store_analysis.c,把本地/线上服务端的真实响应喂进与真机完全
// 相同的解析代码;真机构建里由 meta_store_api.c 调用。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "meta_store_api.h"   // meta_store_analysis_t

// 解析 /api/analyze 响应 JSON 并填充 out(整体清零后填充,不部分信任)。
// 必填:supported / suggestedSlot / reason(存在时必须已知且与 supported 互洽);
// name 与 extracted 仅在 supported=true 时必填(服务端对不可装玩法发
// name:null / extracted:null,见 meta_store_analysis.c 顶部注释)。
// 返回 false = 契约字段缺失/越界/reason 漂移(此时 reason 字段仍保有可展示码)。
bool meta_store_analysis_parse(const char *json, size_t len,
                               meta_store_analysis_t *out);
