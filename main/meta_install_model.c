// main/meta_install_model.c —— 实现见头文件注释。
// 全部为有界纯逻辑:不分配堆、不碰 flash、不依赖 ESP-IDF;host 与真机同一份。
#include "meta_install_model.h"

#include <string.h>
#include <stdio.h>

// 可打印 ASCII(0x20..0x7E):上屏文本与 MNAM 字节集的同一约束
// (meta_name.c 拒绝非可打印字节,这里前置拒绝,免得装完名字悄悄丢失)。
static bool printable_ascii(const char *s)
{
    for (; *s != '\0'; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c < 0x20u || c > 0x7Eu) return false;
    }
    return true;
}

bool meta_install_model_parse(const char *json, size_t len,
                              meta_install_manifest_t *out)
{
    if (!json || len == 0 || !out) return false;

    memset(out, 0, sizeof(*out));
    out->suggested_slot = -1;
    out->phone_slot = -1;

    int64_t v;

    // protocol 必填且必须为 v1。
    if (!meta_store_json_get_int(json, len, "protocol", &v)) return false;
    if (v != META_INSTALL_PROTOCOL_V1) return false;
    out->protocol = (uint8_t)v;

    // playId / revisionId 仅作记录/展示(设备不做 WAN 请求)。
    if (!meta_store_json_get_int(json, len, "playId", &v)) return false;
    if (v < 0 || v > UINT32_MAX) return false;
    out->play_id = (uint32_t)v;

    if (!meta_store_json_get_int(json, len, "revisionId", &v)) return false;
    if (v < 0 || v > UINT32_MAX) return false;
    out->revision_id = (uint32_t)v;

    // name:非空、≤META_NAME_LEN(超长在缓冲处直接失败)、可打印 ASCII。
    if (!meta_store_json_get_string(json, len, "name", out->name, sizeof(out->name))) {
        return false;
    }
    if (out->name[0] == '\0' || !printable_ascii(out->name)) return false;

    // storeSha256:恰好 64 位小写 hex(手机侧已对合并镜像校验;设备只作绑定锚)。
    char hex[META_SHA256_HEX_LEN + 1];
    uint8_t digest[32];
    if (!meta_store_json_get_string(json, len, "storeSha256", hex, sizeof(hex))) {
        return false;
    }
    if (!meta_store_json_parse_sha256(hex, strlen(hex), digest)) return false;
    memcpy(out->store_sha256_hex, hex, sizeof(out->store_sha256_hex));

    // imageLen 必填且 >0。
    if (!meta_store_json_get_int(json, len, "imageLen", &v)) return false;
    if (v <= 0 || v > UINT32_MAX) return false;
    out->image_len = (uint32_t)v;

    // sha256(剥离后 app image):恰好 64 位小写 hex。
    if (!meta_store_json_get_string(json, len, "sha256", hex, sizeof(hex))) return false;
    if (!meta_store_json_parse_sha256(hex, strlen(hex), out->sha256)) return false;

    // suggestedSlot: -1 .. META_SLOT_COUNT-1。
    if (!meta_store_json_get_int(json, len, "suggestedSlot", &v)) return false;
    if (v < -1 || v > META_SLOT_COUNT - 1) return false;
    out->suggested_slot = (int8_t)v;

    // reason:必填短句(文档 §6.4 示例携带;缺字段按合同拒绝,不信任半截 offer)。
    if (!meta_store_json_get_string(json, len, "reason", out->reason,
                                    sizeof(out->reason))) {
        return false;
    }
    if (out->reason[0] == '\0' || !printable_ascii(out->reason)) return false;

    // slots 数组(文档 §6.4;契约里没有数组外的 slots-count 字段,个数由数组本身给出)。
    size_t count = 0;
    if (!meta_store_json_get_array_count(json, len, "slots", &count)) return false;
    if (count == 0 || count > META_SLOT_COUNT) return false;
    out->slots_count = (uint8_t)count;

    for (size_t i = 0; i < count; i++) {
        if (!meta_store_json_get_array_int(json, len, "slots", i, "slot", &v)) {
            return false;
        }
        if (v < 0 || v > META_SLOT_COUNT - 1) return false;
        for (size_t j = 0; j < i; j++) {
            if (out->slots[j].slot == (int8_t)v) return false;   // 槽位重复:拒绝
        }
        out->slots[i].slot = (int8_t)v;

        if (!meta_store_json_get_array_int(json, len, "slots", i, "limit", &v)) {
            return false;
        }
        if (v <= 0 || v > UINT32_MAX) return false;
        out->slots[i].limit = (uint32_t)v;

        if (!meta_store_json_get_array_bool(json, len, "slots", i, "fit",
                                            &out->slots[i].fit)) {
            return false;
        }
    }

    // slot 可选(交互 v2:手机侧选定槽位)。缺省 -1 = 设备物理确认旧流程;
    // 越界即拒绝,防手机侧 bug 把镜像写进不存在的槽。
    if (meta_store_json_get_int(json, len, "slot", &v)) {
        if (v < -1 || v >= META_SLOT_COUNT) return false;
        out->phone_slot = (int8_t)v;
    }

    // carve 提案(dynslot §4.5,可选):carveOffset/carveSize 必须成对出现,
    // 半截提案按形状不合法拒绝;几何对错由 carve_ok 重跑分配器裁定。
    int64_t v_off = 0, v_sz = 0;
    const bool has_off = meta_store_json_get_int(json, len, "carveOffset", &v_off);
    const bool has_sz = meta_store_json_get_int(json, len, "carveSize", &v_sz);
    if (has_off != has_sz) return false;
    if (has_off) {
        if (v_off < 0 || v_off > UINT32_MAX) return false;
        if (v_sz <= 0 || v_sz > UINT32_MAX) return false;
        out->has_carve = true;
        out->carve_offset = (uint32_t)v_off;
        out->carve_size = (uint32_t)v_sz;
    }

    // M5: data[] 迁移声明(可选,仅升级路径携带)
    size_t data_count = 0;
    if (meta_store_json_get_array_count(json, len, "data", &data_count)) {
        if (data_count > META_DATA_MAX) return false;
        out->data_count = (uint8_t)data_count;
        for (size_t i = 0; i < data_count; i++) {
            int64_t v_pid = 0, v_sz2 = 0;
            char label[META_DATA_LABEL_MAX + 1];
            if (!meta_store_json_get_array_int(json, len, "data", i, "playId", &v_pid)) return false;
            if (v_pid < 0 || v_pid > UINT32_MAX) return false;
            if (!meta_store_json_get_array_int(json, len, "data", i, "size", &v_sz2)) return false;
            if (v_sz2 <= 0 || v_sz2 > UINT32_MAX) return false;
            if (!meta_store_json_get_array_string(json, len, "data", i, "label", label, sizeof(label))) return false;
            if (label[0] == '\0') return false;
            out->data[i].play_id = (uint32_t)v_pid;
            out->data[i].size = (uint32_t)v_sz2;
            strncpy(out->data[i].label, label, sizeof(out->data[i].label) - 1);
            out->data[i].label[sizeof(out->data[i].label) - 1] = '\0';
        }
    } else {
        out->data_count = 0;
    }

    return true;
}

bool meta_install_model_parse_session_req(const char *json, size_t len,
                                          meta_install_session_req_t *out)
{
    if (!json || len == 0 || !out) return false;
    memset(out, 0, sizeof(*out));
    out->slot = -1;

    int64_t v;
    if (!meta_store_json_get_int(json, len, "imageLen", &v)) return false;
    if (v <= 0 || v > UINT32_MAX) return false;
    out->image_len = (uint32_t)v;

    char hex[META_SHA256_HEX_LEN + 1];
    if (!meta_store_json_get_string(json, len, "sha256", hex, sizeof(hex))) return false;
    if (!meta_store_json_parse_sha256(hex, strlen(hex), out->sha256)) return false;

    if (!meta_store_json_get_int(json, len, "slot", &v)) return false;
    if (v < 0 || v > META_SLOT_COUNT - 1) return false;
    out->slot = (int8_t)v;
    return true;
}

bool meta_install_model_parse_remove(const char *json, size_t len, int *slot_out)
{
    if (!json || len == 0 || !slot_out) return false;
    int64_t v;
    // 只认 {"slot":N} 的 N(scan_int 拒绝字符串/小数);缺字段即拒,半截请求
    // 不得进入擦除路径(适配层映射 400)。
    if (!meta_store_json_get_int(json, len, "slot", &v)) return false;
    if (v < 0 || v > META_SLOT_COUNT - 1) return false;
    *slot_out = (int)v;
    return true;
}

bool meta_install_model_remove_ok(const meta_carve_t *cur, int slot)
{
    if (!cur) return false;
    if (slot < 0 || slot >= (int)cur->count) return false;
    return true;
}

bool meta_install_model_slot_fit(const meta_install_geom_t *g, int8_t slot,
                                 uint32_t image_len)
{
    if (!g || slot < 0 || slot >= META_SLOT_COUNT) return false;
    const uint32_t limit = g->limit[slot];
    if (limit == 0) return false;          // 分区不存在/未注入
    if (image_len == 0) return false;
    return image_len <= limit;
}

int meta_install_model_carve_ok(const meta_install_manifest_t *m,
                                const meta_carve_t *cur)
{
    if (!m || !cur || !m->has_carve) return -1;
    const uint32_t need = meta_carve_need(m->image_len);
    if (need == 0 || m->carve_size != need) return -1;   // 尺寸必须 = 设备需求

    // 幂等重试:提案槽已存在于当前 carve(重启后手机重发同一 prepare)。
    for (uint8_t i = 0; i < cur->count; i++) {
        const meta_carve_slot_t *s = &cur->slot[i];
        if (s->kind == META_CARVE_KIND_APP && s->offset == m->carve_offset &&
            s->size == m->carve_size) {
            // phone_slot >= 0 必须吻合下标;-1 = 设备物理确认旧流程,下标由设备定。
            if (m->phone_slot >= 0 && m->phone_slot != (int8_t)i) return -1;
            return (int)i;
        }
    }

    // 全新提案:设备重跑 first-fit,落点与下标必须与手机提案逐项吻合。
    meta_carve_t next;
    const int idx = meta_carve_place(cur, need, META_CARVE_KIND_APP, &next);
    if (idx < 0) return -1;
    if (next.slot[idx].offset != m->carve_offset) return -1;
    if (m->phone_slot >= 0 && m->phone_slot != (int8_t)idx) return -1;
    return idx;
}

bool meta_install_geom_from_carve(const meta_carve_t *c,
                                  const meta_install_manifest_t *m,
                                  meta_install_geom_t *g,
                                  int *carve_idx)
{
    if (carve_idx) *carve_idx = -1;
    if (!c || !g || c->count > META_CARVE_MAX_SLOTS) return false;
    memset(g, 0, sizeof(*g));
    for (uint8_t i = 0; i < c->count; i++) {
        if (c->slot[i].kind != META_CARVE_KIND_APP) continue;   // storage 预留不可装(L2)
        g->limit[i] = meta_sign_app_limit(c->slot[i].size);
    }
    if (m && m->has_carve) {
        const int idx = meta_install_model_carve_ok(m, c);
        // 提案成立 → 尚未物化的下标也按提案尺寸准入(否则 offer_ok 会把
        // 手机的 fit 声称当错报拒掉,新槽永远装不进去)。
        if (idx >= 0 && idx < META_SLOT_COUNT) {
            g->limit[idx] = meta_sign_app_limit(m->carve_size);
        }
        if (carve_idx) *carve_idx = idx;
    }
    return true;
}

bool meta_install_model_offer_ok(const meta_install_manifest_t *m,
                                 const meta_install_geom_t *g)
{
    if (!m || !g) return false;
    if (m->protocol != META_INSTALL_PROTOCOL_V1) return false;
    if (m->name[0] == '\0' || m->image_len == 0 || m->slots_count == 0) return false;

    bool any_fit = false;
    for (int i = 0; i < m->slots_count; i++) {
        const bool local = meta_install_model_slot_fit(g, m->slots[i].slot,
                                                       m->image_len);
        // 手机声称 fit 的槽位必须本地也 fit,否则整体拒绝(错报与欺骗同一处理)。
        if (m->slots[i].fit && !local) return false;
        if (m->slots[i].fit && local) any_fit = true;
    }
    return any_fit;
}

int8_t meta_install_model_default_slot(const meta_install_manifest_t *m,
                                       const meta_install_geom_t *g)
{
    if (!m || !g) return -1;
    // 建议槽位只有本地几何也 fit 才作默认(文档 §6.4)。
    if (meta_install_model_slot_fit(g, m->suggested_slot, m->image_len)) {
        return m->suggested_slot;
    }
    for (int i = 0; i < m->slots_count; i++) {
        if (m->slots[i].fit &&
            meta_install_model_slot_fit(g, m->slots[i].slot, m->image_len)) {
            return m->slots[i].slot;
        }
    }
    for (int i = 0; i < m->slots_count; i++) {
        if (meta_install_model_slot_fit(g, m->slots[i].slot, m->image_len)) {
            return m->slots[i].slot;
        }
    }
    return -1;
}

bool meta_install_model_session_ok(const meta_install_manifest_t *m,
                                   int8_t confirmed_slot,
                                   const meta_install_session_req_t *req,
                                   const meta_install_geom_t *g)
{
    if (!m || !req || !g) return false;
    // slot 必须等于设备物理确认的槽位(§8:三字段与已确认 offer 完全一致)。
    if (confirmed_slot < 0 || req->slot != confirmed_slot) return false;
    if (req->image_len != m->image_len) return false;
    if (memcmp(req->sha256, m->sha256, 32) != 0) return false;
    // 确认槽位在 session 开启时再过一次本地几何(分区视图不应变化,双保险)。
    return meta_install_model_slot_fit(g, req->slot, req->image_len);
}

meta_chunk_verdict_t meta_install_model_chunk(bool session_opened,
                                              uint32_t session_offset,
                                              uint32_t expected_len,
                                              uint32_t offset, uint32_t length,
                                              uint32_t max_chunk)
{
    if (!session_opened) return META_CHUNK_REJECT;
    if (length == 0 || length > max_chunk) return META_CHUNK_REJECT;
    if (expected_len != 0) {
        if (session_offset > expected_len) return META_CHUNK_REJECT;   // 状态损坏
        if ((uint64_t)offset + length > expected_len) return META_CHUNK_REJECT;
    }
    if (offset == session_offset) return META_CHUNK_OK;
    // 整段重复 = 幂等成功(文档 §6.5:duplicate already-written offsets are idempotent)。
    if (offset < session_offset && (uint64_t)offset + length <= session_offset) {
        return META_CHUNK_DUP;
    }
    return META_CHUNK_REJECT;   // 跳位 / 回退 / 部分重叠
}

bool meta_install_model_finalize_ready(uint32_t received, uint32_t expected_len)
{
    return expected_len > 0 && received == expected_len;
}
