// tests/test_meta_install_model.c —— meta_install_model 纯逻辑 host 测试。
// 样本以文档 docs/assets/lan-pair-install-design.md §6.4 的 install offer 为基准,
// 锁定:JSON 形状/边界拒绝、本地几何复核、默认槽位回退、session 三字段绑定、
// chunk 顺序/幂等/拒绝、finalize 前置。真机适配层(meta_store_install.c)与
// 本测试链接同一份代码。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meta_install_model.h"

// 文档 §6.4 示例的物化(真实 64 位 hex;含空白/换行以压扫描器)。
static const char OFFER[] =
    "{\n"
    "  \"protocol\": 1,\n"
    "  \"playId\": 563,\n"
    "  \"revisionId\": 1499,\n"
    "  \"name\": \"ai-passport-9\",\n"
    "  \"storeSha256\": \"2956f77bb1db312b5796cf8982bceed66be564d4590441a9fa8abfddb3fbdf01\",\n"
    "  \"imageLen\": 1892032,\n"
    "  \"sha256\": \"1828e251042477c152709dab15a83c2dcc28060eae2e3dc57ff19d3431ad24c4\",\n"
    "  \"suggestedSlot\": 0,\n"
    "  \"slots\": [\n"
    "    { \"slot\": 0, \"limit\": 1921024, \"fit\": true },\n"
    "    { \"slot\": 1, \"limit\": 2093056, \"fit\": true },\n"
    "    { \"slot\": 2, \"limit\": 2740224, \"fit\": true }\n"
    "  ],\n"
    "  \"reason\": \"ok\"\n"
    "}";
#define OFFER_LEN (sizeof(OFFER) - 1)

// 本机几何:与 offer 的 limit 表一致(0 表示分区不存在)。
static const meta_install_geom_t GEOM = { .limit = { 1921024, 2093056, 2740224 } };

// 在副本上做替换的小工具(避免手写整份 JSON);替换全部命中(如 "fit": true
// 在样本里出现 3 次),任一模式未命中返回 false。输出总是以 NUL 结尾。
typedef struct {
    char buf[1024];
    size_t len;                 // 本次替换后的实际长度(替换会改变总长)
} mutated_t;

static bool mutated_offer(mutated_t *m, const char *from, const char *to)
{
    const size_t from_len = strlen(from);
    const size_t to_len = strlen(to);
    if (from_len == 0) return false;
    const char *src = OFFER;
    size_t w = 0;
    bool hit_any = false;
    for (;;) {
        const char *hit = strstr(src, from);
        if (!hit) break;
        const size_t chunk = (size_t)(hit - src);
        if (w + chunk + to_len + 1 >= sizeof(m->buf)) return false;
        memcpy(m->buf + w, src, chunk);
        w += chunk;
        memcpy(m->buf + w, to, to_len);
        w += to_len;
        src = hit + from_len;
        hit_any = true;
    }
    const size_t tail = OFFER_LEN - (size_t)(src - OFFER);
    if (w + tail + 1 > sizeof(m->buf)) return false;
    memcpy(m->buf + w, src, tail);
    w += tail;
    m->buf[w] = '\0';
    m->len = w;
    return hit_any;
}

static void test_parse_happy(void)
{
    meta_install_manifest_t m;
    mutated_t mu;
    assert(meta_install_model_parse(OFFER, OFFER_LEN, &m));
    assert(m.protocol == META_INSTALL_PROTOCOL_V1);
    assert(m.play_id == 563);
    assert(m.revision_id == 1499);
    assert(strcmp(m.name, "ai-passport-9") == 0);
    assert(m.image_len == 1892032);
    assert(m.suggested_slot == 0);
    assert(m.slots_count == 3);
    assert(m.slots[0].slot == 0 && m.slots[0].limit == 1921024 && m.slots[0].fit);
    assert(m.slots[1].slot == 1 && m.slots[1].limit == 2093056 && m.slots[1].fit);
    assert(m.slots[2].slot == 2 && m.slots[2].limit == 2740224 && m.slots[2].fit);
    assert(strcmp(m.reason, "ok") == 0);
    assert(m.sha256[0] == 0x18 && m.sha256[31] == 0xc4);
    // 交互 v2:slot 可选,缺省 -1(设备物理确认旧流程)。
    assert(m.phone_slot == -1);
    assert(mutated_offer(&mu, "  \"suggestedSlot\": 0,", "  \"suggestedSlot\": 0, \"slot\": 2,"));
    assert(meta_install_model_parse(mu.buf, mu.len, &m));
    assert(m.phone_slot == 2);                                      // 手机选定槽位
    printf("PASS parse happy path\n");
}

static void test_parse_rejects(void)
{
    meta_install_manifest_t m;
    mutated_t mu;

    // 必填字段逐个缺失都必须整体拒绝。
    assert(mutated_offer(&mu, "\"protocol\": 1", "\"protocol\": 2"));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 版本不符
    assert(mutated_offer(&mu, "\"playId\": 563,", ""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 缺 playId
    assert(mutated_offer(&mu, "\"revisionId\": 1499,", ""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 缺 revisionId
    assert(mutated_offer(&mu, "\"imageLen\": 1892032,", ""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 缺 imageLen
    assert(mutated_offer(&mu, "\"suggestedSlot\": 0,", ""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 缺 suggestedSlot
    assert(mutated_offer(&mu, "\"reason\": \"ok\"", "\"reason\": \"\""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 空 reason
    assert(mutated_offer(&mu, "\"sha256\": \"1828e251042477c152709dab15a83c2dcc"
                              "28060eae2e3dc57ff19d3431ad24c4\"",
                         "\"sha256\": \"1828e251042477c152709dab15a83c2dcc\""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 短 sha
    assert(mutated_offer(&mu, "\"name\": \"ai-passport-9\"",
                         "\"name\": \"AI Passport 汉字名\""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 非 ASCII 名
    assert(mutated_offer(&mu, "\"name\": \"ai-passport-9\"",
                         "\"name\": \"abcdefghijklmnopqrstuvwxyz0123456789\""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 超 32 字节名
    assert(mutated_offer(&mu, "\"limit\": 1921024", "\"limit\": 0"));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // limit <= 0
    assert(mutated_offer(&mu, "{ \"slot\": 2, \"limit\": 2740224, \"fit\": true }",
                         "{ \"slot\": 0, \"limit\": 2740224, \"fit\": true }"));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 槽位重复
    assert(mutated_offer(&mu, "\"suggestedSlot\": 0", "\"suggestedSlot\": 8"));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 建议槽位越界(META_SLOT_COUNT=8)
    assert(mutated_offer(&mu, "  \"suggestedSlot\": 0,", "  \"suggestedSlot\": 0, \"slot\": 8,"));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 手机选定槽位越界
    assert(mutated_offer(&mu, "  \"suggestedSlot\": 0,", "  \"suggestedSlot\": 0, \"slot\": -2,"));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // slot < -1 非法
    assert(mutated_offer(&mu, "  \"reason\": \"ok\"\n", ""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 缺 reason
    assert(!meta_install_model_parse(OFFER, OFFER_LEN - 3, &m));     // 截断
    assert(!meta_install_model_parse("", 0, &m));                    // 空输入
    assert(!meta_install_model_parse(NULL, OFFER_LEN, &m));          // NULL

    // 数组个数边界:空数组拒绝;契约里不存在的 slots-count 字段不被要求。
    assert(mutated_offer(&mu, "{ \"slot\": 0, \"limit\": 1921024, \"fit\": true },\n"
                              "    { \"slot\": 1, \"limit\": 2093056, \"fit\": true },\n"
                              "    { \"slot\": 2, \"limit\": 2740224, \"fit\": true }",
                         ""));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // slots 空
    printf("PASS parse rejects\n");
}

static void test_offer_ok(void)
{
    meta_install_manifest_t m;
    assert(meta_install_model_parse(OFFER, OFFER_LEN, &m));
    assert(meta_install_model_offer_ok(&m, &GEOM));

    // 手机声称 fit 但本地几何不够 → 整体拒绝。
    const meta_install_geom_t small = { .limit = { 1000000, 0, 0 } };
    assert(!meta_install_model_offer_ok(&m, &small));

    // 本地全无上限(分区不存在)→ 拒绝。
    const meta_install_geom_t none = { .limit = { 0, 0, 0 } };
    assert(!meta_install_model_offer_ok(&m, &none));

    // 手机全部声称 unfit(设备端无从确认可装)→ 拒绝。
    mutated_t mu;
    assert(mutated_offer(&mu, "\"fit\": true", "\"fit\": false"));
    meta_install_manifest_t m2;
    assert(meta_install_model_parse(mu.buf, mu.len, &m2));
    assert(!meta_install_model_offer_ok(&m2, &GEOM));

    assert(!meta_install_model_offer_ok(NULL, &GEOM));
    printf("PASS offer_ok\n");
}

static void test_default_slot(void)
{
    meta_install_manifest_t m;
    assert(meta_install_model_parse(OFFER, OFFER_LEN, &m));

    // 建议槽位本地 fit → 用建议。
    assert(meta_install_model_default_slot(&m, &GEOM) == 0);

    // 建议槽位本地不够 → 退回首个手机 fit 且本地 fit 的槽位。
    const meta_install_geom_t no_slot0 = { .limit = { 0, 2093056, 2740224 } };
    assert(meta_install_model_default_slot(&m, &no_slot0) == 1);

    // 建议槽位分区不存在且手机 fit 表也全落空 → 任意本地 fit 兜底。
    mutated_t mu;
    assert(mutated_offer(&mu, "\"fit\": true", "\"fit\": false"));
    meta_install_manifest_t m2;
    assert(meta_install_model_parse(mu.buf, mu.len, &m2));
    const meta_install_geom_t only2 = { .limit = { 0, 0, 2740224 } };
    assert(meta_install_model_default_slot(&m2, &only2) == 2);

    // 全部不可装 → -1。
    assert(meta_install_model_default_slot(&m, NULL) == -1);
    printf("PASS default slot\n");
}

static void test_session_ok(void)
{
    meta_install_manifest_t m;
    assert(meta_install_model_parse(OFFER, OFFER_LEN, &m));

    meta_install_session_req_t req = {
        .image_len = 1892032,
        .slot = 1,
    };
    memcpy(req.sha256, m.sha256, 32);
    // 确认槽位 1 时,session 槽位必须也是 1。
    assert(!meta_install_model_session_ok(&m, 0, &req, &GEOM));
    assert(meta_install_model_session_ok(&m, 1, &req, &GEOM));

    // imageLen 不符 → 拒绝。
    req.image_len = 1892033;
    assert(!meta_install_model_session_ok(&m, 1, &req, &GEOM));
    req.image_len = 1892032;

    // sha256 不符 → 拒绝。
    req.sha256[0] ^= 0xFF;
    assert(!meta_install_model_session_ok(&m, 1, &req, &GEOM));
    req.sha256[0] ^= 0xFF;

    // 未确认(-1)或槽位越界 → 拒绝。
    assert(!meta_install_model_session_ok(&m, -1, &req, &GEOM));
    req.slot = 7;
    assert(!meta_install_model_session_ok(&m, 1, &req, &GEOM));
    req.slot = 1;

    // 确认槽位本地无上限 → 拒绝。
    const meta_install_geom_t no_slot1 = { .limit = { 1921024, 0, 2740224 } };
    assert(!meta_install_model_session_ok(&m, 1, &req, &no_slot1));
    printf("PASS session binding\n");
}

static void test_chunk(void)
{
    const uint32_t LEN = 1892032;

    // 顺序写入。
    assert(meta_install_model_chunk(true, 0, LEN, 0, 65536, 65536)
           == META_CHUNK_OK);
    assert(meta_install_model_chunk(true, 65536, LEN, 65536, 65536, 65536)
           == META_CHUNK_OK);
    // 末块(不足 max_chunk 的尾块)。
    assert(meta_install_model_chunk(true, LEN - 100, LEN, LEN - 100, 100, 65536)
           == META_CHUNK_OK);

    // 幂等重复:整段已写过 → DUP(成功,跳过写)。
    assert(meta_install_model_chunk(true, 65536, LEN, 0, 65536, 65536)
           == META_CHUNK_DUP);
    // 部分重叠(回退但越界已写水位)→ 拒绝。
    assert(meta_install_model_chunk(true, 65536, LEN, 32768, 65536, 65536)
           == META_CHUNK_REJECT);
    // 跳位 → 拒绝。
    assert(meta_install_model_chunk(true, 0, LEN, 65536, 65536, 65536)
           == META_CHUNK_REJECT);

    // 越过 imageLen → 拒绝。
    assert(meta_install_model_chunk(true, LEN - 10, LEN, LEN - 10, 100, 65536)
           == META_CHUNK_REJECT);
    // 零长 / 超 max_chunk → 拒绝。
    assert(meta_install_model_chunk(true, 0, LEN, 0, 0, 65536) == META_CHUNK_REJECT);
    assert(meta_install_model_chunk(true, 0, LEN, 0, 65537, 65536)
           == META_CHUNK_REJECT);
    // 未开 session → 拒绝(上传不得先于确认,§8)。
    assert(meta_install_model_chunk(false, 0, LEN, 0, 4096, 65536)
           == META_CHUNK_REJECT);
    printf("PASS chunk verdicts\n");
}

static void test_parse_session_req(void)
{
    meta_install_session_req_t req;
    static const char OK_BODY[] =
        "{\"imageLen\":1892032,"
        "\"sha256\":\"1828e251042477c152709dab15a83c2dcc28060eae2e3dc57ff19d3431ad24c4\","
        "\"slot\":0}";
    assert(meta_install_model_parse_session_req(OK_BODY, sizeof(OK_BODY) - 1, &req));
    assert(req.image_len == 1892032);
    assert(req.slot == 0);
    assert(req.sha256[0] == 0x18 && req.sha256[31] == 0xc4);

    // 缺字段 / 类型错 / 槽位越界 / 短 sha 一律拒绝。
    static const char NO_LEN[] =
        "{\"sha256\":\"1828e251042477c152709dab15a83c2dcc28060eae2e3dc57ff19d3431ad24c4\","
        "\"slot\":0}";
    assert(!meta_install_model_parse_session_req(NO_LEN, sizeof(NO_LEN) - 1, &req));
    static const char BAD_SLOT[] =
        "{\"imageLen\":1892032,"
        "\"sha256\":\"1828e251042477c152709dab15a83c2dcc28060eae2e3dc57ff19d3431ad24c4\","
        "\"slot\":8}";
    assert(!meta_install_model_parse_session_req(BAD_SLOT, sizeof(BAD_SLOT) - 1, &req));
    static const char SHORT_SHA[] =
        "{\"imageLen\":1892032,\"sha256\":\"1828\",\"slot\":0}";
    assert(!meta_install_model_parse_session_req(SHORT_SHA, sizeof(SHORT_SHA) - 1, &req));
    assert(!meta_install_model_parse_session_req(NULL, 10, &req));
    printf("PASS session req parse\n");
}

// ---- dynslot 显式删除(design §4.5 Remove:HTTP /api/install/remove) ----

static void test_parse_remove(void)
{
    meta_install_remove_req_t r;

    // slot 必填;eraseData 可选(缺省 = 归档)。
    const char *j1 = "{\"slot\":3}";
    assert(meta_install_model_parse_remove(j1, strlen(j1), &r));
    assert(r.slot == 3);
    assert(!r.erase_data);   // 缺省 = 归档(design §6)

    const char *j2 = "{ \"slot\" : 0 }";
    assert(meta_install_model_parse_remove(j2, strlen(j2), &r));
    assert(r.slot == 0);

    // eraseData:true → 显式擦除;false → 归档。
    const char *j3 = "{\"slot\":2,\"eraseData\":true}";
    assert(meta_install_model_parse_remove(j3, strlen(j3), &r));
    assert(r.slot == 2 && r.erase_data);

    const char *j4 = "{\"slot\":2,\"eraseData\":false}";
    assert(meta_install_model_parse_remove(j4, strlen(j4), &r));
    assert(r.slot == 2 && !r.erase_data);

    // 非布尔 eraseData 落到更安全的归档侧(不误擦)。
    const char *j5 = "{\"slot\":2,\"eraseData\":\"yes\"}";
    assert(meta_install_model_parse_remove(j5, strlen(j5), &r));
    assert(r.slot == 2 && !r.erase_data);

    // 缺 slot / 越界 / 负数 / 非整数 / 空输入一律拒绝(不动 *out)。
    assert(!meta_install_model_parse_remove("{\"x\":1}", 7, &r));
    assert(!meta_install_model_parse_remove("{\"slot\":8}", 10, &r));    // >= META_SLOT_COUNT
    assert(!meta_install_model_parse_remove("{\"slot\":-1}", 11, &r));
    assert(!meta_install_model_parse_remove("{\"slot\":\"3\"}", 12, &r));
    assert(!meta_install_model_parse_remove("{\"slot\":3.5}", 12, &r));
    assert(!meta_install_model_parse_remove("", 0, &r));
    assert(!meta_install_model_parse_remove(NULL, 10, &r));
    printf("PASS remove req parse\n");
}

static void test_remove_ok(void)
{
    meta_carve_t c;
    memset(&c, 0, sizeof(c));
    c.count = 3;

    // carve 内下标可删(含末位;storage 预留同样可回收)。
    assert(meta_install_model_remove_ok(&c, 0));
    assert(meta_install_model_remove_ok(&c, 2));
    // 越界 / 负数 / 无 carve → 拒(适配层映射 404,不擦不提交)。
    assert(!meta_install_model_remove_ok(&c, 3));
    assert(!meta_install_model_remove_ok(&c, -1));
    assert(!meta_install_model_remove_ok(NULL, 0));

    // 全新设备(count=0)无槽可删。
    c.count = 0;
    assert(!meta_install_model_remove_ok(&c, 0));
    printf("PASS remove verdict\n");
}

static void test_finalize_ready(void)
{
    assert(meta_install_model_finalize_ready(1892032, 1892032));
    assert(!meta_install_model_finalize_ready(1892031, 1892032));
    assert(!meta_install_model_finalize_ready(0, 0));
    assert(!meta_install_model_finalize_ready(0, 100));
    printf("PASS finalize precheck\n");
}

// ---- dynslot carve 提案(design §4.5:手机提案,设备重跑分配器拒绝分歧) ----

// 参数化 offer:构造含可选 carve 提案与选定槽位的最小合法 manifest。
static bool build_carve_offer(char *buf, size_t cap, uint32_t image_len,
                              const char *carve_json, int slot)
{
    static const char zero_hex[65] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    const int n = snprintf(buf, cap,
        "{\"protocol\":1,\"playId\":1,\"revisionId\":1,\"name\":\"p\","
        "\"storeSha256\":\"%s\",\"imageLen\":%u,\"sha256\":\"%s\","
        "\"suggestedSlot\":0,\"slot\":%d,%s"
        "\"slots\":[{\"slot\":0,\"limit\":1921024,\"fit\":true}],"
        "\"reason\":\"ok\"}",
        zero_hex, image_len, zero_hex, slot,
        (carve_json && carve_json[0]) ? carve_json : "");
    return n > 0 && (size_t)n < cap;
}

static void test_parse_carve(void)
{
    char js[1024];
    meta_install_manifest_t m;

    // 缺省:无提案(复用现有槽流程,零额外重启)。
    assert(build_carve_offer(js, sizeof(js), 0x1F000, "", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(!m.has_carve);

    // 成对出现 → 解析成功。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(m.has_carve);
    assert(m.carve_offset == 1572864u);   // 0x180000
    assert(m.carve_size == 131072u);      // 0x20000

    // 半截提案(只有 offset 或只有 size)→ 整体拒绝。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,", 0));
    assert(!meta_install_model_parse(js, strlen(js), &m));
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveSize\":131072,", 0));
    assert(!meta_install_model_parse(js, strlen(js), &m));
    // 负 size → 拒绝。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":-1,", 0));
    assert(!meta_install_model_parse(js, strlen(js), &m));
    printf("PASS carve parse\n");
}

static void test_carve_ok(void)
{
    char js[1024];
    meta_install_manifest_t m;
    meta_carve_t cur;
    memset(&cur, 0, sizeof(cur));

    // 全新设备(空 carve):提案必须与设备 first-fit 逐项吻合。
    // image_len 0x1F000 → need = max(128KB, align4k(+4KB 尾)) = 0x20000。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == 0);

    // 分歧:offset 不是设备 first-fit 落点 → 拒(手机几何与设备不一致)。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1703936,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == -1);

    // size != 设备算出的 need → 拒。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":196608,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == -1);

    // phone_slot != 落点下标 → 拒。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == -1);

    // 已有一个槽(slot0@0x180000/0x20000):
    //   - 提案与已存在 slot0 同几何 → 幂等复用(重启后重发 prepare 的情形);
    //   - 新提案必须落在 first-fit 的 0x1A0000。
    assert(meta_carve_place(&cur, 0x20000, META_CARVE_KIND_APP, &cur) == 0);
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == 0);    // 幂等:同几何 slot0 复用
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1703936,\"carveSize\":131072,", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == 1);

    // 幂等重试:提案槽已在 carve 中(重启后手机重发同一 prepare)→ 同下标放行,
    // 不得再放一个新槽。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == 0);
    // 幂等路径也必须核对 phone_slot 下标。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == -1);

    // 8 槽上限:填满后新提案 → 拒(设备重跑分配器无处可放)。
    meta_carve_t full = cur;   // slot0@0x180000 + slot1@0x1A0000
    while (full.count < META_CARVE_MAX_SLOTS) {
        assert(meta_carve_place(&full, 0x20000, META_CARVE_KIND_APP, &full) >= 0);
    }
    assert(full.count == META_CARVE_MAX_SLOTS);
    // 幂等:slot0 提案在满载下仍放行(重试语义不因满载失效)。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &full) == 0);
    // 新提案(槽不存在且满载无处可放)→ 拒。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":3801088,\"carveSize\":131072,", 7));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &full) == -1);

    // 大镜像(need > pool_0 容量)→ first-fit 跨段落 pool_1 起点 0x360000。
    // image_len 0x48F000 → need = 0x490000;提案 offset 0x360000 / size 0x490000。
    meta_carve_t empty;
    memset(&empty, 0, sizeof(empty));
    assert(build_carve_offer(js, sizeof(js), 0x48F000,
                             "\"carveOffset\":3538944,\"carveSize\":4784128,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &empty) == 0);
    printf("PASS carve proposal re-check\n");
}

// 设备确认旧流程:手机给提案但不选槽(phone_slot = -1)→ 落点仍由设备
// 分配器裁定;只有 >= 0 的 phone_slot 才参与下标比对。
static void test_carve_ok_device_confirm(void)
{
    char js[1024];
    meta_install_manifest_t m;
    meta_carve_t cur;
    memset(&cur, 0, sizeof(cur));

    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", -1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(m.phone_slot == -1);
    assert(meta_install_model_carve_ok(&m, &cur) == 0);   // 空 carve:first-fit 落 0

    // 分歧(offset 不是设备 first-fit 落点)仍拒。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1703936,\"carveSize\":131072,", -1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == -1);

    // 幂等复用(提案槽已在 carve 中)也放行。
    assert(meta_carve_place(&cur, 0x20000, META_CARVE_KIND_APP, &cur) == 0);
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", -1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_carve_ok(&m, &cur) == 0);
    printf("PASS carve ok (device-confirm flow)\n");
}

// dynslot 本地上限:由规范 carve(+可选在途提案)派生,替代"分区存在与否"
// (新槽尚未物化时分区视图看不见它 —— 提案下标按提案尺寸预先计入)。
static void test_geom_from_carve(void)
{
    meta_install_geom_t g;
    meta_carve_t c;
    memset(&c, 0, sizeof(c));
    int idx = 99;

    // 无槽无提案 → 全 0(全新设备:尚无可装槽)。
    assert(meta_install_geom_from_carve(&c, NULL, &g, &idx));
    assert(idx == -1);
    for (int i = 0; i < META_SLOT_COUNT; i++) assert(g.limit[i] == 0);

    // 既有槽 → 上限 = meta_sign_app_limit(槽尺寸)(与分区视图同一约定)。
    assert(meta_carve_place(&c, 0x20000, META_CARVE_KIND_APP, &c) == 0);
    assert(meta_install_geom_from_carve(&c, NULL, &g, &idx));
    assert(g.limit[0] == meta_sign_app_limit(0x20000));
    assert(g.limit[1] == 0);

    // 在途提案(新槽)成立 → carve_ok 裁定下标,该下标按提案尺寸计入上限。
    char js[1024];
    meta_install_manifest_t m;
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1703936,\"carveSize\":131072,", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_geom_from_carve(&c, &m, &g, &idx));
    assert(idx == 1);
    assert(g.limit[0] == meta_sign_app_limit(0x20000));
    assert(g.limit[1] == meta_sign_app_limit(0x20000));

    // 提案分歧 → 下标 -1,未分配下标不给任何上限(offer_ok 会拒 fit 声称)。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1835008,\"carveSize\":131072,", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_geom_from_carve(&c, &m, &g, &idx));
    assert(idx == -1);
    assert(g.limit[1] == 0);

    // NULL 参数 → false。
    assert(!meta_install_geom_from_carve(NULL, NULL, &g, NULL));
    assert(!meta_install_geom_from_carve(&c, NULL, NULL, NULL));
    printf("PASS geom from carve\n");
}

static void test_place_offer(void)
{
    char js[2048];
    meta_install_manifest_t m;
    meta_carve_t cur, next;
    int idx = -1;
    bool changed = false;
    char label[META_DATA_LABEL_MAX + 1];
    meta_install_no_fit_t nf;
    memset(&cur, 0, sizeof(cur));

    // image 0x1F000 → need 0x20000(与 carve_ok 用例同源几何)。
    // ── 全新放置:槽位 + 两条数据,一次 OK ──
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
        "\"carveOffset\":1572864,\"carveSize\":131072,"
        "\"data\":[{\"playId\":1,\"size\":4096,\"subtype\":130,\"initialImageSize\":123,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"label\":\"rec\"},"
        "{\"playId\":1,\"size\":8192,\"label\":\"cfg\"}],", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(m.data_count == 2);
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_OK);
    assert(idx == 0 && changed);
    assert(next.count == 1 && next.slot[0].offset == 0x180000);
    assert(next.slot[0].play_id == 1);
    assert(next.data_count == 2);
    assert(next.data[0].play_id == 1 && next.data[0].state == META_DATA_PRISTINE);
    assert(next.data[0].subtype == 130);
    assert(strcmp(next.data[0].label, "rec") == 0);
    // 数据 first-fit 落在槽位之后(pool_0 先填):rec 紧邻槽尾(0x1A0000);
    // 数据偏移按 META_CARVE_OFFSET_ALIGN(64KB)对齐 → cfg 跳到下一 64KB 界。
    assert(next.data[0].offset == 0x1A0000);
    assert(next.data[1].offset == 0x1B0000);
    // 纯槽位、无数据 → OK。
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_OK);
    assert(next.data_count == 0 && changed);

    // ── 幂等:提案槽已在 carve → 副本=现状,changed=false ──
    assert(meta_carve_place(&cur, 0x20000, META_CARVE_KIND_APP, &cur) == 0);
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1572864,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_OK);
    assert(idx == 0 && !changed);
    assert(memcmp(&next, &cur, sizeof(next)) == 0);

    // ── 幂等 + 缺失数据补放:changed=true(旧固件只放槽位的老数据迁移) ──
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
        "\"carveOffset\":1572864,\"carveSize\":131072,"
        "\"data\":[{\"playId\":1,\"size\":4096,\"label\":\"rec\"}],", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_OK);
    assert(idx == 0 && changed);
    assert(next.count == 1 && next.data_count == 1);
    assert(next.data[0].play_id == 1 && strcmp(next.data[0].label, "rec") == 0);

    // ── unsupported data subtype → REJECTED ──
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
        "\"carveOffset\":1703936,\"carveSize\":131072,"
        "\"data\":[{\"playId\":1,\"size\":4096,\"subtype\":64,\"label\":\"custom\"}],", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_REJECTED);
    assert(strcmp(label, "custom") == 0);

    // ── 保留标签 → REJECTED ──
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
        "\"carveOffset\":1703936,\"carveSize\":131072,"
        "\"data\":[{\"playId\":1,\"size\":4096,\"label\":\"nvs\"}],", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_REJECTED);
    assert(strcmp(label, "nvs") == 0);

    // ── manifest 内重复 label → REJECTED ──
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
        "\"carveOffset\":1703936,\"carveSize\":131072,"
        "\"data\":[{\"playId\":1,\"size\":4096,\"label\":\"rec\"},"
        "{\"playId\":1,\"size\":4096,\"label\":\"rec\"}],", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_REJECTED);

    // ── 升级保留:既有 (play_id,label) 记录不重复放置 ──
    {
        meta_carve_t with_data = cur;
        uint32_t off = 0;
        assert(meta_carve_place_data(&with_data, 0x2000, &off));
        meta_carve_data_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.play_id = 1; rec.offset = off; rec.size = 0x2000;
        rec.state = META_DATA_DIRTY; rec.type = 1; rec.subtype = 0x82;
        strncpy(rec.label, "rec", sizeof(rec.label) - 1);
        assert(meta_carve_data_append(&with_data, &rec));
        // 提案槽必须避开既有数据记录(rec@0x1A0000+0x2000,64KB 对齐) → 0x1B0000。
        assert(build_carve_offer(js, sizeof(js), 0x1F000,
            "\"carveOffset\":1769472,\"carveSize\":131072,"
            "\"data\":[{\"playId\":1,\"size\":8192,\"label\":\"rec\"},"
            "{\"playId\":1,\"size\":4096,\"label\":\"cfg\"}],", 1));
        assert(meta_install_model_parse(js, strlen(js), &m));
        assert(meta_install_model_place_offer(&m, &with_data, &next, &idx,
                                              &changed, label, &nf) == META_PLACE_OK);
        // rec 保留原偏移/DIRTY;只有 cfg 新放。
        assert(next.count == 2 && next.data_count == 2);
        assert(meta_carve_find_data(&next, 1, "rec") >= 0);
        const meta_carve_data_t *kept = &next.data[meta_carve_find_data(&next, 1, "rec")];
        assert(kept->offset == off && kept->state == META_DATA_DIRTY);
        assert(meta_carve_find_data(&next, 1, "cfg") >= 0);
        assert(next.data[meta_carve_find_data(&next, 1, "cfg")].state == META_DATA_PRISTINE);

        // ── 升级 DATA 扩容:不能原地扩大,必须找到不重叠的新 extent。
        meta_carve_t grow = with_data;
        const int grow_idx = meta_carve_find_data(&grow, 7, "rec");
        assert(grow_idx >= 0);
        const uint32_t old_off = grow.data[grow_idx].offset;
        assert(build_carve_offer(js, sizeof(js), 0x1F000,
            "\"carveOffset\":1769472,\"carveSize\":131072,"
            "\"data\":[{\"playId\":1,\"size\":16384,\"label\":\"rec\"}],", 1));
        assert(meta_install_model_parse(js, strlen(js), &m));
        assert(meta_install_model_place_offer(&m, &grow, &next, &idx,
                                              &changed, label, &nf) == META_PLACE_OK);
        const int grown_idx = meta_carve_find_data(&next, 1, "rec");
        assert(grown_idx >= 0);
        assert(next.data[grown_idx].size == 0x4000);
        assert(next.data[grown_idx].offset != old_off);
        assert(next.data[grown_idx].state == META_DATA_DIRTY);
    }

    // ── NO_FIT_SLOT:满载 → 数字(largestGap=0,回收量按状态拆) ──
    {
        meta_carve_t full = cur;
        while (full.count < META_CARVE_MAX_SLOTS) {
            assert(meta_carve_place(&full, 0x20000, META_CARVE_KIND_APP, &full) >= 0);
        }
        // 塞一条 ARCHIVED + 一条 PRISTINE 数据,验证拆分。
        uint32_t off = 0;
        assert(meta_carve_place_data(&full, 0x1000, &off));
        meta_carve_data_t rec; memset(&rec, 0, sizeof(rec));
        rec.play_id = 9; rec.offset = off; rec.size = 0x1000;
        rec.state = META_DATA_ARCHIVED; rec.type = 1; rec.subtype = 1;
        strncpy(rec.label, "old", sizeof(rec.label) - 1);
        assert(meta_carve_data_append(&full, &rec));
        assert(meta_carve_place_data(&full, 0x2000, &off));
        memset(&rec, 0, sizeof(rec));
        rec.play_id = 9; rec.offset = off; rec.size = 0x2000;
        rec.state = META_DATA_PRISTINE; rec.type = 1; rec.subtype = 1;
        strncpy(rec.label, "new", sizeof(rec.label) - 1);
        assert(meta_carve_data_append(&full, &rec));

        assert(build_carve_offer(js, sizeof(js), 0x1F000,
                                 "\"carveOffset\":6291456,\"carveSize\":131072,", 7));
        assert(meta_install_model_parse(js, strlen(js), &m));
        assert(meta_install_model_place_offer(&m, &full, &next, &idx, &changed,
                                              label, &nf) == META_PLACE_NO_FIT_SLOT);
        assert(nf.needed == 0x20000);
        // 8 槽上限触发的失败:空间其实充足(pool1 全空 0x49E000),数字如实上报
        // —— 槽位耗尽与空间耗尽由 UI 结合列表 count 区分。
        assert(nf.largest_gap == 0x49E000);
        assert(nf.reclaimable_archived == 0x1000);
        assert(nf.reclaimable_pristine == 0x2000);
    }

    // ── NO_FIT_DATA:槽位放得下但数据无洞 ──
    {
        // 用数据记录把 pool_0 的洞填满,只剩槽位 itself 后的窄缝不够 4KB?直接
        // 用超大尺寸数据声明:need=0x20000 槽位后,数据要 0x100000,池剩余不足。
        meta_carve_t roomy = cur;   // 1 槽 @0x180000+0x20000
        assert(build_carve_offer(js, sizeof(js), 0x1F000,
            "\"carveOffset\":1703936,\"carveSize\":131072,"
            "\"data\":[{\"playId\":1,\"size\":6291456,\"label\":\"big\"}],", 1));
        assert(meta_install_model_parse(js, strlen(js), &m));
        assert(meta_install_model_place_offer(&m, &roomy, &next, &idx, &changed,
                                              label, &nf) == META_PLACE_NO_FIT_DATA);
        assert(strcmp(label, "big") == 0);
        assert(nf.needed == 6291456);
        assert(nf.largest_gap < 6291456);
    }

    // ── 形状/分歧拒绝 ──
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1703936,\"carveSize\":131072,", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_REJECTED);  // 下标分歧
    assert(build_carve_offer(js, sizeof(js), 0x1F000,
                             "\"carveOffset\":1703936,\"carveSize\":196608,", 1));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_REJECTED);  // size != need
    // NULL/无提案 → REJECTED。
    assert(meta_install_model_place_offer(NULL, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_REJECTED);
    assert(build_carve_offer(js, sizeof(js), 0x1F000, "", 0));
    assert(meta_install_model_parse(js, strlen(js), &m));
    assert(meta_install_model_place_offer(&m, &cur, &next, &idx, &changed,
                                          label, &nf) == META_PLACE_REJECTED);  // 无提案

    printf("PASS place offer\n");
}


static void test_geom_merge_carve(void)
{
    meta_install_geom_t g;
    memset(&g, 0, sizeof(g));

    // 空 carve → 无合并。
    meta_install_geom_merge_carve(&g, NULL);
    assert(g.limit[0] == 0);

    meta_carve_t c;
    memset(&c, 0, sizeof(c));
    assert(meta_carve_place(&c, 0x20000, META_CARVE_KIND_APP, &c) == 0);
    assert(meta_carve_place(&c, 0x40000, META_CARVE_KIND_APP, &c) == 1);

    // 全未命中(缓存视角空)→ 两槽都按 carve 几何补;limit = size − 4KB 尾扇区。
    meta_install_geom_merge_carve(&g, &c);
    assert(g.limit[0] == meta_sign_app_limit(0x20000));
    assert(g.limit[1] == meta_sign_app_limit(0x40000));
    assert(g.limit[2] == 0);

    // 部分命中:缓存已有 slot0 的 limit → 不覆盖;slot1 仍补。
    memset(&g, 0, sizeof(g));
    g.limit[0] = 12345;   // 模拟缓存值(异常值也能证"不覆盖")
    meta_install_geom_merge_carve(&g, &c);
    assert(g.limit[0] == 12345);
    assert(g.limit[1] == meta_sign_app_limit(0x40000));

    // storage 预留槽(L2)不可装 → 不补。
    meta_carve_t s;
    memset(&s, 0, sizeof(s));
    assert(meta_carve_place(&s, 0x20000, META_CARVE_KIND_STORAGE, &s) == 0);
    memset(&g, 0, sizeof(g));
    meta_install_geom_merge_carve(&g, &s);
    assert(g.limit[0] == 0);

    printf("PASS geom merge carve\n");
}

int main(void)
{
    test_parse_happy();
    test_parse_rejects();
    test_offer_ok();
    test_default_slot();
    test_session_ok();
    test_parse_session_req();
    test_chunk();
    test_finalize_ready();
    test_parse_carve();
    test_carve_ok();
    test_carve_ok_device_confirm();
    test_geom_from_carve();
    test_parse_remove();
    test_remove_ok();
    test_place_offer();
    test_geom_merge_carve();
    printf("ALL meta_install_model TESTS PASSED\n");
    return 0;
}