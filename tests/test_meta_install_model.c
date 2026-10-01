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
    assert(mutated_offer(&mu, "\"suggestedSlot\": 0", "\"suggestedSlot\": 3"));
    assert(!meta_install_model_parse(mu.buf, mu.len, &m));        // 建议槽位越界
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
        "\"slot\":3}";
    assert(!meta_install_model_parse_session_req(BAD_SLOT, sizeof(BAD_SLOT) - 1, &req));
    static const char SHORT_SHA[] =
        "{\"imageLen\":1892032,\"sha256\":\"1828\",\"slot\":0}";
    assert(!meta_install_model_parse_session_req(SHORT_SHA, sizeof(SHORT_SHA) - 1, &req));
    assert(!meta_install_model_parse_session_req(NULL, 10, &req));
    printf("PASS session req parse\n");
}

static void test_finalize_ready(void)
{
    assert(meta_install_model_finalize_ready(1892032, 1892032));
    assert(!meta_install_model_finalize_ready(1892031, 1892032));
    assert(!meta_install_model_finalize_ready(0, 0));
    assert(!meta_install_model_finalize_ready(0, 100));
    printf("PASS finalize precheck\n");
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
    printf("ALL meta_install_model TESTS PASSED\n");
    return 0;
}
