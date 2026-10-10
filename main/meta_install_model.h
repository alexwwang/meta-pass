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
#include "meta_carve.h"        // dynslot carve 提案复核(§4.5 设备权威)
#include "meta_sign.h"         // meta_sign_app_limit(槽尺寸 → 可装上限)
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
    int8_t   phone_slot;                        // >=0 = 手机侧已选定槽位(交互 v2:选槽/确认
                                                // 都在手机完成,设备跳过物理确认直达上传;
                                                // 旧固件忽略未知 JSON 字段 → 自动回退设备选槽);
                                                // -1 = 设备侧物理确认(旧流程)
    // dynslot carve 提案(design §4.5,可选):手机为"新槽"计算的提案。
    // carveOffset/carveSize 必须成对出现;缺省 = 无提案(复用现有槽)。
    // 设备用 meta_install_model_carve_ok 重跑分配器拒绝几何分歧(L4)。
    bool     has_carve;
    uint32_t carve_offset;                      // 提案槽偏移(64KB 对齐,由分配器复核)
    uint32_t carve_size;                        // 提案槽尺寸(必须 = meta_carve_need(image_len))
    uint8_t  slots_count;                       // 下方 slots 长度(1..META_SLOT_COUNT)
    struct {
        int8_t   slot;                          // 槽位编号(0..META_SLOT_COUNT-1,不重复)
        uint32_t limit;                         // 该槽位本地上限(手机侧可装判断依据)
        bool     fit;                           // 手机侧判断该槽位可装
    } slots[META_SLOT_COUNT];
    char     reason[32];                        // 手机侧判断短句(ok / custom-partitions / ...)
    // M5: 数据分区迁移(design §4.1,可选)
    uint8_t  data_count;                        // 下方 data[] 长度(0..META_DATA_MAX)
    struct {
        uint32_t play_id;                       // 归属玩法 id(>0 有效;0 = 忽略)
        uint32_t size;                          // 数据分区 required_size
        uint8_t  subtype;                       // ESP-IDF data subtype
        uint32_t initial_image_size;            // initial payload bytes; 0 = empty
        uint8_t  sha256[32];                    // SHA-256 of initial payload; zero when size=0
        char     label[META_DATA_LABEL_MAX + 1]; // 子固件分区标签(≤16B)
    } data[META_DATA_MAX];
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

// 设备本地几何(适配层注入;0 = 分区不存在/无可用上限)。
// dynslot:上限由规范 carve 派生(见 meta_install_geom_from_carve)—— 新槽在
// 物化前分区视图看不见,靠在途提案下标预先计入,提案不成立的下标仍是 0。
typedef struct {
    uint32_t limit[META_SLOT_COUNT];            // meta_sign_app_limit(槽尺寸)
} meta_install_geom_t;

// P0-5 方案B:prepare 物化新槽位后,当前启动的 esp_partition 表缓存看不见
// 它(首次访问后驻留 SRAM)。本函数把 carve 记录里的 APP 槽位合入 geom:
// limit[i]==0(缓存未命中)且 carve 第 i 槽是 APP → 用 carve 几何补。
// 设备确认流(confirm/default_slot/offer_ok/session_ok 全走 geom_refresh)
// 因此能对"本次 prepare 刚 carve、尚未复位"的槽位完成确认与上传校验。
void meta_install_geom_merge_carve(meta_install_geom_t *g, const meta_carve_t *c);

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

// dynslot carve 提案复核(design §4.5:手机提案,设备重跑分配器拒绝分歧):
//   - carve_size 必须 = meta_carve_need(m->image_len)(设备侧需求);
//   - 提案槽已在 cur 中(重启后手机重发的幂等路径)→ 且 phone_slot == 其下标;
//   - 否则设备跑 first-fit 放置,落点偏移与 phone_slot 必须与提案逐项吻合;
//   - 任何分歧/无处可放 → -1(适配层拒绝 offer)。
// 返回 >=0 = 提案成立时的槽位下标。
// phone_slot == -1(设备物理确认旧流程)时不参与下标比对:落点仍由设备分配器
// 裁定,最终确认哪个槽由 P3/提交时再约束。
int meta_install_model_carve_ok(const meta_install_manifest_t *m,
                                const meta_carve_t *cur);

// ── P0-5 提案物化(见 docs/assets/dynslot-install-data-wiring-design.md) ──

// no-fit 上报数字(§4 JSON):needed/largestGap + 按授权级别拆分的可回收量。
typedef struct {
    uint32_t needed;               // 所需字节(slot = meta_carve_need;data = 记录尺寸)
    uint32_t largest_gap;          // 最大单块可分配区间(决策时点的 carve)
    uint32_t reclaimable_archived; // tier 3 可自动回收(ARCHIVED)
    uint32_t reclaimable_pristine; // tier 4 需用户同意(PRISTINE)
} meta_install_no_fit_t;

typedef enum {
    META_PLACE_OK = 0,    // 成功;*out_changed = out_next 是否异于 cur(决定是否需提交+复位)
    META_PLACE_NO_FIT_SLOT,  // 槽位放不下;out_nf 填数字
    META_PLACE_NO_FIT_DATA,  // 数据条目放不下;out_label/out_nf 填
    META_PLACE_REJECTED,     // 形状/分歧/保留标签/重复标签/数组满;out_label 可填
} meta_install_place_verdict_t;

// 在副本上重放逐条目放置(纯函数:不动 s_carve,不提交):
//   1. 形状校验(carve_size == meta_carve_need,protocol);幂等扫描(提案槽已在
//      carve → 副本=现状,下标返回;缺失的数据记录仍补放并置 *out_changed);
//   2. meta_carve_place 放槽位(先放;落点/phone_slot 与提案分歧 → REJECTED);
//   3. 逐条 manifest.data[]:play_id==0 跳过;(play_id,label) 已存在 → 保留
//      (升级路径,finalize 的 data_copy 负责迁移);保留标签/重复标签 → REJECTED;
//      place_data + append(PRISTINE);任一条放不下 → NO_FIT_DATA(整体失败,副本丢弃)。
// out_label 缓冲 ≥ META_DATA_LABEL_MAX+1;NO_FIT_DATA/REJECTED(标签类)时填失败条目。
// REJECTED 不区分原因码,调用方统一 400(细分对 UI 无决策价值)。
meta_install_place_verdict_t meta_install_model_place_offer(
    const meta_install_manifest_t *m, const meta_carve_t *cur,
    meta_carve_t *out_next, int *out_idx, bool *out_changed,
    char *out_label, meta_install_no_fit_t *out_nf);

// APP 卸载请求体: {"slot":N},N ∈ [0, META_SLOT_COUNT)。
// APP 删除必然级联删除全部关联 DATA；不提供单独删除或保留 DATA 的用户选项。
// 返回 false 时适配层映射 400,任何擦除/提交不得先于它发生。
typedef struct {
    int  slot;
    bool erase_data;   // true = 用户显式"删除数据"(擦字节 + 移除记录)
} meta_install_remove_req_t;

bool meta_install_model_parse_remove(const char *json, size_t len,
                                     meta_install_remove_req_t *out);

// 删除可行性(纯判定,设备权威的一部分):slot 必须在规范 carve 内
// (0 <= slot < cur->count)。false → 适配层映射 404,不擦不提交、seq 不动。
// 空 carve(全新设备 count=0)无槽可删;storage 预留槽同样可回收(L2)。
bool meta_install_model_remove_ok(const meta_carve_t *cur, int slot);

// dynslot 本地上限派生(design §4.5,替代"分区存在与否"的几何注入):
//   - 既有 APP 槽 → meta_sign_app_limit(slot.size)(storage 预留不计, L2);
//   - 未分配下标 → 0;若 m 带 carve 提案且 carve_ok 成立 → 该下标按
//     meta_sign_app_limit(carve_size) 预填(槽尚未物化,提案先行准入);
//   - carve_idx(可 NULL)回传 carve_ok 结果:-1 = 无提案/提案不成立。
// 语义与分区视图等价:表 == carve 是不变量(hook 与 ensure 双向对账)。
bool meta_install_geom_from_carve(const meta_carve_t *c,
                                  const meta_install_manifest_t *m,  // 可为 NULL
                                  meta_install_geom_t *g,
                                  int *carve_idx);

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
