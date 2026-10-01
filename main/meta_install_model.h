// main/meta_install_model.h —— LAN 手机辅助安装 offer/session 判定的纯逻辑
// (零 ESP-IDF 依赖,host 测试 tests/test_meta_install_model.c 链接同一份代码)。
//
// 契约来源:docs/assets/lan-pair-install-design.md
//   §6.4 install offer 的 JSON 形状与设备确认前复核;
//   §6.5 session/chunk 的绑定与偏移规则;
//   §8   imageLen/sha256/slot 必须与已确认 offer 完全一致、上传不先于物理确认。
// 设备侧 meta_store_install.c 只负责注入本地分区几何(本模块只比对数字)与
// 执行 flash/OTA 副作用;凡"拒绝"在本模块判定,不在适配层散落重复。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meta_slots.h"        // META_SLOT_COUNT / META_NAME_LEN / META_SHA256_HEX_LEN
#include "meta_store_json.h"

// v1 manifest 协议版本;offer.protocol 必须等于它(boot 页 loader 的兼容性握手
// 也以设备上报的此值为准)。
#define META_INSTALL_PROTOCOL_V1 1

// 手机单 chunk 上限(文档 §6.5 示例 65536;设备内部再按 flash 写尺度拆块)。
#define META_INSTALL_MAX_CHUNK 65536u

// 手机提交的 install offer / manifest(文档 §6.4 JSON 的物化结构)。
// 字段名与 JSON 一一对应;解析全部定长,拒绝越界与非可打印 ASCII(上屏 + MNAM 契约)。
typedef struct {
    uint8_t  protocol;                          // 必须 = META_INSTALL_PROTOCOL_V1
    uint32_t play_id;                           // 市场 play id(展示/日志用)
    uint32_t revision_id;                       // 市场 revision(展示/日志用)
    char     name[META_NAME_LEN + 1];           // 显示名(≤32,可打印 ASCII;MNAM 同源)
    char     store_sha256_hex[META_SHA256_HEX_LEN + 1]; // 合并镜像 sha256(手机侧已校验)
    uint32_t image_len;                         // 剥离后 app image 字节数
    uint8_t  sha256[32];                        // 剥离后 app image SHA-256(上传终点比对)
    int8_t   suggested_slot;                    // 手机建议槽位;-1 = 无
    uint8_t  slots_count;                       // 下方 slots 长度(1..META_SLOT_COUNT)
    struct {
        int8_t   slot;                          // 槽位编号(0..META_SLOT_COUNT-1,不重复)
        uint32_t limit;                         // 该槽位本地上限(手机侧可装判断依据)
        bool     fit;                           // 手机侧判断该槽位可装
    } slots[META_SLOT_COUNT];
    char     reason[32];                        // 手机侧判断短句(ok / custom-partitions / ...)
} meta_install_manifest_t;

// session 开启请求(文档 §6.5 session body 的三个绑定字段)。
typedef struct {
    uint32_t image_len;                         // 必须 = 已确认 manifest.image_len
    uint8_t  sha256[32];                        // 必须 = 已确认 manifest.sha256
    int8_t   slot;                              // 必须 = 设备物理确认的槽位
} meta_install_session_req_t;

// session body 解析(文档 §6.5:imageLen/sha256/slot 三字段,缺一不可)。
bool meta_install_model_parse_session_req(const char *json, size_t len,
                                          meta_install_session_req_t *out);

// 设备本地分区几何(适配层注入;0 = 分区不存在/无可用上限)。
typedef struct {
    uint32_t limit[META_SLOT_COUNT];            // meta_sign_app_limit(part->size)
} meta_install_geom_t;

// chunk 偏移判定结果(文档 §6.5)。
typedef enum {
    META_CHUNK_REJECT = 0,  // 未开 session / 跳位 / 回退 / 越过 imageLen / 超长
    META_CHUNK_OK,          // 顺序下一偏移:写入
    META_CHUNK_DUP,         // 整段已写过的重复:幂等成功,跳过写
} meta_chunk_verdict_t;

// ---- 解析与判定(纯函数) ----

// 解析 offer JSON:形状/边界/字符集。任何偏差返回 false 且不改 out。
// 不看本地上限(那一步在 offer_ok),但字段完整性在此一次收齐。
bool meta_install_model_parse(const char *json, size_t len,
                              meta_install_manifest_t *out);

// 设备确认前复核(文档 §6.4 步骤 1 + §8):
//   - 至少一个「手机声明 fit 且本地几何也 fit」的槽位;
//   - 手机每一个 fit 声称都必须本地 fit(声称不符即整体拒绝,防错报/欺骗)。
bool meta_install_model_offer_ok(const meta_install_manifest_t *m,
                                 const meta_install_geom_t *g);

// 本地几何:槽位存在且 image_len <= 上限。
bool meta_install_model_slot_fit(const meta_install_geom_t *g, int8_t slot,
                                 uint32_t image_len);

// 默认选中槽位:suggestedSlot 本地 fit 才用,否则首个「手机 fit 且本地 fit」,
// 再退化到任意本地 fit;-1 = 无(offer_ok 已拒绝这种情况)。
int8_t meta_install_model_default_slot(const meta_install_manifest_t *m,
                                       const meta_install_geom_t *g);

// session 三字段与「已确认 offer」完全一致(§8)。confirmed_slot 是设备物理确认
// 的槽位(必须本地 fit);imageLen/sha256 逐字节比对 manifest。
bool meta_install_model_session_ok(const meta_install_manifest_t *m,
                                   int8_t confirmed_slot,
                                   const meta_install_session_req_t *req,
                                   const meta_install_geom_t *g);

// chunk 偏移判定(§6.5):offset 必须等于已写偏移;整段重复幂等跳过;
// 跳位/回退/部分重叠/越过 imageLen/零长/超 max_chunk 一律拒绝。
meta_chunk_verdict_t meta_install_model_chunk(bool session_opened,
                                              uint32_t session_offset,
                                              uint32_t expected_len,
                                              uint32_t offset, uint32_t length,
                                              uint32_t max_chunk);

// finalize 前置(§6.5 步骤 1):已收字节数恰等于 imageLen 且非零。
bool meta_install_model_finalize_ready(uint32_t received, uint32_t expected_len);
