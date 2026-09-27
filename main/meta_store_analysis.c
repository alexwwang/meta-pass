// main/meta_store_analysis.c —— /api/analyze 响应体解析(纯逻辑,无 IDF 依赖)。
// 独立编译单元:与 meta_store_json.c 同模式,host 合同测试
// (tests/test_store_analyze_contract.c)直接链接本文件,把本地/线上服务端的
// 真实响应喂进与真机完全相同的解析代码;真机构建里由 meta_store_api.c 调用。
// 契约:tools/install-slot/server.mjs /api/analyze 输出 = 方案文档 §1.2。
#include "meta_store_api.h"

#include <stdio.h>
#include <string.h>

#include "meta_name.h"
#include "meta_store_json.h"

// 服务端下发的原因码(与 tools/install-slot/store-analyze.js 契约一致)。
// custom-partitions 双态:supported=false = 硬拒(陌生数据分区,装了必坏);
// supported=true  = 警告可继续(subtype 0x40 自定义区,镜像不含该分区内容,
// 运行时若真读写会缺存储 —— P2 页显示警告,由用户决定)。
static const struct {
    const char *reason;
    bool        supported;
} k_reasons[] = {
    { "ok",                true  },
    { "not-found",         false },
    { "unavailable",       false },
    { "format",            false },
    { "no-factory",        false },
    { "wrong-chip",        false },
    { "custom-partitions", false },   // supported 值仅约束硬拒分支;警告分支 true 也合法
    { "too-large",         false },
};

// 从响应 JSON 填充 analysis。必填:supported / suggestedSlot / reason(存在时必须
// 已知且与 supported 互洽);name 与 extracted 仅在 supported=true 时必填 ——
// 服务端对不可装玩法发 name:null / extracted:null(sha256 惰性计算,store-analyze.js
// 错误规范化路径),设备端必须仍解析出 reason 码,否则 too-large/wrong-chip 等
// 真实原因被吞成 format/unavailable(r5 契约初版踩坑,合同测试锁定)。
// 返回 false = 契约字段缺失/越界(不部分信任;调用方把 reason 展示到屏上)。
bool meta_store_analysis_parse(const char *json, size_t len, meta_store_analysis_t *out)
{
    int64_t v;

    memset(out, 0, sizeof(*out));
    out->suggested_slot = -1;
    snprintf(out->reason, sizeof(out->reason), "%s", "format");

    if (!meta_store_json_get_bool(json, len, "supported", &out->supported)) {
        return false;
    }
    if (!meta_store_json_get_int(json, len, "suggestedSlot", &v)
        || v < -1 || v > META_SLOT_COUNT - 1) {
        return false;
    }
    out->suggested_slot = (int8_t)v;

    // reason 可选(缺席保持 format);存在则必须在已知集合内(防服务端契约漂移
    // 被静默吞掉),且与 supported 互洽。custom-partitions 是双态码(硬拒 false /
    // 警告可继续 true),两种 supported 值都合法,由 detail 有无与 UI 分支区分。
    char reason[24] = {0};
    if (meta_store_json_get_string(json, len, "reason", reason, sizeof(reason))) {
        snprintf(out->reason, sizeof(out->reason), "%s", reason);
        bool known = false;
        for (size_t i = 0; i < sizeof(k_reasons) / sizeof(k_reasons[0]); i++) {
            if (strcmp(reason, k_reasons[i].reason) == 0) {
                known = true;
                if (strcmp(reason, "custom-partitions") != 0
                    && out->supported != k_reasons[i].supported) {
                    return false;
                }
                break;
            }
        }
        if (!known) return false;
    }
    // 警告补充参数(如 custom-partitions 警告的自定义分区名);缺席 = 空串。
    // supported=true + custom-partitions = 警告可继续(方案 r8;见头文件注释)。
    char detail[24] = {0};
    if (meta_store_json_get_string(json, len, "detail", detail, sizeof(detail))) {
        snprintf(out->detail, sizeof(out->detail), "%s", detail);
    }

    // 可装玩法:安装要用的字段全部必填(name 超限拒收的理由见 meta_name.h ——
    // MNAM blob 只收 32 字节,半途而废的镜像宁可不安)。
    if (out->supported) {
        char sha_hex[META_SHA256_HEX_LEN + 1];
        if (!meta_store_json_get_string(json, len, "name", out->name, sizeof(out->name))) {
            return false;
        }
        if (strlen(out->name) > META_NAME_MAX) {
            return false;
        }
        if (!meta_store_json_get_int(json, len, "extracted/imageLen", &v)
            || v <= 0 || v > UINT32_MAX) {
            return false;
        }
        out->image_len = (uint32_t)v;
        if (!meta_store_json_get_string(json, len, "extracted/sha256",
                                        sha_hex, sizeof(sha_hex))) {
            return false;
        }
        if (!meta_store_json_parse_sha256(sha_hex, strlen(sha_hex), out->sha256)) {
            return false;
        }
    }
    return true;
}
