// tests/test_meta_net_upload.c —— meta_net.c h_upload 的 host 集成测试。
//
// 设计:
// - 通过 #include "../main/meta_net.c" 直接编译真实 handler,静态函数与静态状态
//   (s_paired / s_code / s_mi / s_slots) 均可直接读写。
// - ESP-IDF API 全部由 tests/esp_stubs/ 下的桩头替代(-Itests/esp_stubs 优先)。
// - meta_store_slot_partition / meta_store_erase_slot 在本文件实现,背后用 RAM 数组
//   模拟 flash(初始 0xFF,erase 填 0xFF,write 拷字节)。
// - mbedtls/sha256.h 桩是真实 FIPS 180-4 实现,先用 "abc" 标准向量自证,再用于断言
//   上传摘要正确性。
// - httpd_query_key_value 桩按 ESP-IDF 5.5.3 真实行为实现:不做 %XX URL 解码
//   (见 esp_http_server/src/httpd_parse.c:882,文档明确 "components are not URLdecoded")。
//   测试直接传未编码 dispname,验证 handler 的过滤/截断逻辑。
//
// 覆盖:配对门禁、槽位/尺寸校验、首块头预检、中断恢复、成功路径(含 sha256 断言)、
// dispname blob 写入/过滤/截断/回退、碎片化接收、单次会话语义。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// 直接包含被测实现:静态函数 h_upload/h_session 与状态 s_paired/s_mi/s_slots 可直接访问。
#include "../main/meta_net.c"

// ---- 测试框架 ----

static int g_fails = 0;
static const char *g_test = "";

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL [%s] line %d: %s\n", g_test, __LINE__, #cond); \
        g_fails++; \
    } \
} while (0)

#define TEST(name) do { g_test = (name); } while (0)

// ---- RAM Flash 模拟器 ----

static uint8_t ram_flash_0[0x1D6000];
static uint8_t ram_flash_1[0x200000];
static uint8_t ram_flash_2[0x29E000];
static uint8_t * const ram_flash[META_SLOT_COUNT] = { ram_flash_0, ram_flash_1, ram_flash_2 };
static const uint32_t ram_flash_sizes[META_SLOT_COUNT] = { 0x1D6000, 0x200000, 0x29E000 };

static esp_partition_t slot_partitions[META_SLOT_COUNT] = {
    { .size = 0x1D6000, .label = "ota_0" },
    { .size = 0x200000, .label = "ota_1" },
    { .size = 0x29E000, .label = "ota_2" },
};

static int partition_to_slot(const esp_partition_t *p)
{
    for (int i = 0; i < META_SLOT_COUNT; i++) {
        if (&slot_partitions[i] == p) return i;
    }
    return -1;
}

// ---- 桩状态(计数器 + 失败注入) ----

static int ota_begin_count;
static int ota_abort_count;
static int ota_write_count;
static size_t ota_write_bytes;   /* 累计写入字节数(滚动头预检后 >0) */
static int ota_end_count;
static int erase_slot_count;
static bool ota_write_fail_next;
static bool ota_end_fail_next;
static struct { int slot; size_t offset; bool active; } ota_ctx;
static esp_app_desc_t injected_desc;
static bool desc_inject_valid;

// ---- meta_store 桩(meta_store.c 不编译,在此实现) ----

const esp_partition_t *meta_store_slot_partition(int slot)
{
    if (slot < 0 || slot >= META_SLOT_COUNT) return NULL;
    return &slot_partitions[slot];
}

esp_err_t meta_store_erase_slot(int slot)
{
    if (slot < 0 || slot >= META_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    erase_slot_count++;
    memset(ram_flash[slot], 0xFF, ram_flash_sizes[slot]);
    return ESP_OK;
}

// ---- esp_partition 桩 ----

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t size)
{
    int slot = partition_to_slot(p);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    if (off + size > ram_flash_sizes[slot]) return ESP_ERR_INVALID_SIZE;
    memset(ram_flash[slot] + off, 0xFF, size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t size)
{
    int slot = partition_to_slot(p);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    if (off + size > ram_flash_sizes[slot]) return ESP_ERR_INVALID_SIZE;
    memcpy(ram_flash[slot] + off, src, size);
    return ESP_OK;
}

// ---- esp_ota 桩 ----

esp_err_t esp_ota_begin(const esp_partition_t *p, size_t image_size, esp_ota_handle_t *out)
{
    (void)image_size;
    int slot = partition_to_slot(p);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    ota_begin_count++;
    ota_ctx.slot = slot;
    ota_ctx.offset = 0;
    ota_ctx.active = true;
    *out = 1;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    (void)handle;
    ota_write_count++;
    ota_write_bytes += size;
    if (ota_write_fail_next) { ota_write_fail_next = false; return ESP_FAIL; }
    if (!ota_ctx.active) return ESP_ERR_INVALID_STATE;
    memcpy(ram_flash[ota_ctx.slot] + ota_ctx.offset, data, size);
    ota_ctx.offset += size;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    (void)handle;
    ota_end_count++;
    if (ota_end_fail_next) { ota_end_fail_next = false; return ESP_FAIL; }
    ota_ctx.active = false;
    return ESP_OK;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    (void)handle;
    ota_abort_count++;
    ota_ctx.active = false;
    return ESP_OK;
}

esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *desc)
{
    (void)p;
    if (!desc_inject_valid) return ESP_ERR_NOT_FOUND;
    memcpy(desc, &injected_desc, sizeof(*desc));
    return ESP_OK;
}

// ---- httpd 桩 ----

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    const char *q = strchr(r->uri, '?');
    if (!q || !q[1]) return ESP_ERR_NOT_FOUND;
    size_t len = strlen(q + 1);
    if (len >= buf_len) return ESP_ERR_HTTPD_RESULT_TRUNC;
    memcpy(buf, q + 1, len + 1);
    return ESP_OK;
}

// 忠实复刻 ESP-IDF 5.5.3 httpd_parse.c:882 行为:不做 %XX 解码,纯字符串查找。
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    if (!qry || !key || !val) return ESP_ERR_INVALID_ARG;
    size_t klen = strlen(key);
    const char *p = qry;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            if (vlen >= val_size) return ESP_ERR_HTTPD_RESULT_TRUNC;
            memcpy(val, v, vlen);
            val[vlen] = '\0';
            return ESP_OK;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    snprintf(r->resp_status, sizeof(r->resp_status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str)
{
    r->resp_body_len = snprintf(r->resp_body, sizeof(r->resp_body), "%s", str);
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    r->recv_call_count++;
    if (r->recv_fail_at_call > 0 && r->recv_call_count == r->recv_fail_at_call)
        return 0;  // 模拟对端断开
    size_t remaining = r->recv_buf_len - r->recv_offset;
    size_t n = buf_len < remaining ? buf_len : remaining;
    if (r->recv_chunk_size > 0 && n > r->recv_chunk_size)
        n = r->recv_chunk_size;
    memcpy(buf, r->recv_buf + r->recv_offset, n);
    r->recv_offset += n;
    return (int)n;
}

// ---- 测试辅助 ----

static meta_slot_info_t test_slots[META_SLOT_COUNT];

static void setup(void)
{
    s_slots = test_slots;
    s_paired = false;
    mi_init(&s_mi);
    memset(test_slots, 0, sizeof(test_slots));
    memset(s_code, 0, sizeof(s_code));
    for (int i = 0; i < META_SLOT_COUNT; i++)
        memset(ram_flash[i], 0xFF, ram_flash_sizes[i]);
    ota_begin_count = 0;
    ota_abort_count = 0;
    ota_write_count = 0;
    ota_write_bytes = 0;
    ota_end_count = 0;
    erase_slot_count = 0;
    ota_write_fail_next = false;
    ota_end_fail_next = false;
    memset(&ota_ctx, 0, sizeof(ota_ctx));
    desc_inject_valid = false;
    memset(&injected_desc, 0, sizeof(injected_desc));
}

// 构造最小合法 ESP32-C3 镜像头(24 字节)
static void make_valid_header(uint8_t *buf)
{
    memset(buf, 0xAB, 24);
    buf[0] = 0xE9;   // magic
    buf[1] = 3;      // segment count
    buf[12] = 0x05;  // chip_id low byte
    buf[13] = 0x00;  // chip_id high byte
}

// 构造完整 payload:合法头 + 确定性填充
static void make_payload(uint8_t *buf, size_t len)
{
    make_valid_header(buf);
    for (size_t i = 24; i < len; i++)
        buf[i] = (uint8_t)(i & 0xFF);
}

// 独立计算 SHA-256 hex(用同一桩,但独立调用路径)
static void compute_sha256_hex(const uint8_t *data, size_t len, char hex[65])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
}

// 构造一个上传请求
static httpd_req_t make_upload_req(int slot, const char *dispname,
                                    const uint8_t *payload, size_t payload_len)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    if (dispname)
        snprintf(req.uri, sizeof(req.uri), "/api/upload?slot=%d&dispname=%s", slot, dispname);
    else
        snprintf(req.uri, sizeof(req.uri), "/api/upload?slot=%d", slot);
    req.content_len = payload_len;
    req.recv_buf = (const char *)payload;
    req.recv_buf_len = payload_len;
    req.recv_offset = 0;
    return req;
}

// 直接进入已配对状态
static void force_paired(void)
{
    s_paired = true;
    s_mi.state = MI_PAIRED;
}

// ---- 测试用例 ----

// a. SHA-256 桩自证:"abc" → 已知标准向量
static void test_sha256_self(void)
{
    TEST("a_sha256_self");
    const char *abc = "abc";
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)abc, 3);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
}

// b. 未配对直接上传 → 403,esp_ota_begin 未被调用
static void test_unpaired_403(void)
{
    TEST("b_unpaired_403");
    setup();
    uint8_t payload[24];
    make_valid_header(payload);
    httpd_req_t req = make_upload_req(0, NULL, payload, sizeof(payload));
    h_upload(&req);
    CHECK(strcmp(req.resp_status, "403 Forbidden") == 0);
    CHECK(strcmp(req.resp_body, "pair first") == 0);
    CHECK(ota_begin_count == 0);
}

// c. h_session 错误码 → 403;正确码 → 200, s_paired=true
static void test_session_pairing(void)
{
    TEST("c_session_pairing");
    setup();
    memcpy(s_code, "123456", 7);
    mi_handle(&s_mi, MI_EV_AP_READY);  // MI_IDLE → MI_AP_UP

    httpd_req_t req1;
    memset(&req1, 0, sizeof(req1));
    snprintf(req1.uri, sizeof(req1.uri), "/api/session?code=000000");
    h_session(&req1);
    CHECK(strcmp(req1.resp_status, "403 Forbidden") == 0);
    CHECK(!s_paired);

    httpd_req_t req2;
    memset(&req2, 0, sizeof(req2));
    snprintf(req2.uri, sizeof(req2.uri), "/api/session?code=123456");
    h_session(&req2);
    CHECK(strcmp(req2.resp_body, "paired") == 0);
    CHECK(s_paired);
    CHECK(s_mi.state == MI_PAIRED);
}

// d. slot=9 → 400;缺 slot 参数 → 400
static void test_slot_validation(void)
{
    TEST("d_slot_validation");
    setup();
    force_paired();
    uint8_t payload[24];
    make_valid_header(payload);

    // slot=9 越界
    httpd_req_t req1 = make_upload_req(9, NULL, payload, sizeof(payload));
    h_upload(&req1);
    CHECK(strcmp(req1.resp_status, "400 Bad Request") == 0);
    CHECK(strcmp(req1.resp_body, "bad slot") == 0);

    // 缺 slot 参数
    httpd_req_t req2;
    memset(&req2, 0, sizeof(req2));
    snprintf(req2.uri, sizeof(req2.uri), "/api/upload");
    req2.content_len = sizeof(payload);
    req2.recv_buf = (const char *)payload;
    req2.recv_buf_len = sizeof(payload);
    h_upload(&req2);
    CHECK(strcmp(req2.resp_status, "400 Bad Request") == 0);
    CHECK(strcmp(req2.resp_body, "bad slot") == 0);
}

// e. content_len=0 → 413;三槽各自上限边界(slot0 与 slot2 上限不同)
static void test_content_length_limits(void)
{
    TEST("e_content_length_limits");
    uint8_t payload[24];
    make_valid_header(payload);

    // content_len=0 → 413
    setup(); force_paired();
    httpd_req_t req0 = make_upload_req(0, NULL, payload, sizeof(payload));
    req0.content_len = 0;
    h_upload(&req0);
    CHECK(strcmp(req0.resp_status, "413 Payload Too Large") == 0);

    // slot0 上限 = 0x1D6000 - 8192 = 0x1D4000;上限+1 → 413
    setup(); force_paired();
    httpd_req_t req1 = make_upload_req(0, NULL, payload, sizeof(payload));
    req1.content_len = 0x1D4001;
    h_upload(&req1);
    CHECK(strcmp(req1.resp_status, "413 Payload Too Large") == 0);

    // slot0 content_len = 上限 → 接受(非 413;会因数据不足而 400,但证明没被尺寸拒绝)
    setup(); force_paired();
    httpd_req_t req2 = make_upload_req(0, NULL, payload, sizeof(payload));
    req2.content_len = 0x1D4000;
    h_upload(&req2);
    CHECK(strcmp(req2.resp_status, "413 Payload Too Large") != 0);

    // slot2 上限 = 0x29E000 - 8192 = 0x29C000,与 slot0 不同
    // slot2 接受 0x1D4001(超过 slot0 上限但不超过 slot2 上限)
    setup(); force_paired();
    httpd_req_t req3 = make_upload_req(2, NULL, payload, sizeof(payload));
    req3.content_len = 0x1D4001;
    h_upload(&req3);
    CHECK(strcmp(req3.resp_status, "413 Payload Too Large") != 0);

    // slot2 content_len = 0x29C001 → 413
    setup(); force_paired();
    httpd_req_t req4 = make_upload_req(2, NULL, payload, sizeof(payload));
    req4.content_len = 0x29C001;
    h_upload(&req4);
    CHECK(strcmp(req4.resp_status, "413 Payload Too Large") == 0);
}
// f. 首块 magic 错 → 400 'upload broken',abort=1, 槽位 INVALID
// 滚动头预检:边收边写,凑齐 24B 才校验。坏头至多白写一个头的量(<=24B)。
static void test_bad_magic(void)
{
    TEST("f_bad_magic");
    setup();
    force_paired();
    uint8_t payload[24];   // 恰好一个头的量
    make_valid_header(payload);
    payload[0] = 0x00;  // 破坏 magic
    httpd_req_t req = make_upload_req(0, NULL, payload, sizeof(payload));
    h_upload(&req);
    CHECK(strcmp(req.resp_status, "400 Bad Request") == 0);
    CHECK(strcmp(req.resp_body, "upload broken") == 0);
    CHECK(ota_abort_count == 1);
    CHECK(ota_write_bytes <= 24);   // 滚动头预检:至多白写一个头
    CHECK(s_slots[0].state == META_SLOT_INVALID);
}

// g1. 第 3 次 recv 返回 0 → abort + INVALID
// payload 2049 字节:第 1 次收 1024,第 2 次收 1024,第 3 次只需 1 字节但返回 0
static void test_recv_failure(void)
{
    TEST("g1_recv_failure");
    setup();
    force_paired();
    uint8_t payload[2049];
    make_payload(payload, sizeof(payload));
    httpd_req_t req = make_upload_req(0, NULL, payload, sizeof(payload));
    req.recv_fail_at_call = 3;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "upload broken") == 0);
    CHECK(ota_abort_count == 1);
    CHECK(s_slots[0].state == META_SLOT_INVALID);
}

// g2. esp_ota_write 失败注入 → abort + INVALID
static void test_ota_write_failure(void)
{
    TEST("g2_ota_write_failure");
    setup();
    force_paired();
    uint8_t payload[1024];
    make_payload(payload, sizeof(payload));
    ota_write_fail_next = true;
    httpd_req_t req = make_upload_req(0, NULL, payload, sizeof(payload));
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "upload broken") == 0);
    CHECK(ota_abort_count == 1);
    CHECK(s_slots[0].state == META_SLOT_INVALID);
}

// g3. esp_ota_end 失败注入 → erase_slot=1 + INVALID
static void test_ota_end_failure(void)
{
    TEST("g3_ota_end_failure");
    setup();
    force_paired();
    uint8_t payload[1024];
    make_payload(payload, sizeof(payload));
    ota_end_fail_next = true;
    httpd_req_t req = make_upload_req(0, NULL, payload, sizeof(payload));
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "verify failed") == 0);
    CHECK(erase_slot_count == 1);
    CHECK(s_slots[0].state == META_SLOT_INVALID);
}

// h. 成功路径:1.5 块 payload(1536 字节),含 sha256 断言
static void test_success_path(void)
{
    TEST("h_success_path");
    setup();
    force_paired();
    const size_t plen = 1536;
    uint8_t payload[plen];
    make_payload(payload, plen);
    httpd_req_t req = make_upload_req(1, NULL, payload, plen);
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(!s_paired);

    // RAM flash 内容与 payload 逐字节相等
    CHECK(memcmp(ram_flash[1], payload, plen) == 0);

    // sha256_hex 与独立参考一致
    char expected_hex[65];
    compute_sha256_hex(payload, plen, expected_hex);
    CHECK(strcmp(s_slots[1].sha256_hex, expected_hex) == 0);

    // s_slots[1] VALID 且 size 正确
    CHECK(s_slots[1].state == META_SLOT_VALID);
    CHECK(s_slots[1].size == (uint32_t)plen);
}

// i1. dispname='LEO RADIO' → blob 在 分区大小-4096 处可解出
static void test_dispname_blob(void)
{
    TEST("i1_dispname_blob");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    httpd_req_t req = make_upload_req(0, "LEO RADIO", payload, plen);
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(strcmp(s_slots[0].name, "LEO RADIO") == 0);

    // 验证 blob 在正确位置且 meta_name_unpack 可解出原名
    const uint32_t blob_off = 0x1D6000 - 4096;
    char unpacked[META_NAME_MAX + 1];
    CHECK(meta_name_unpack(ram_flash[0] + blob_off, 64, unpacked, sizeof(unpacked)));
    CHECK(strcmp(unpacked, "LEO RADIO") == 0);
}

// i2. dispname 含 0x01/0xE9 → 被剔除
static void test_dispname_filter(void)
{
    TEST("i2_dispname_filter");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.uri, sizeof(req.uri), "/api/upload?slot=0&dispname=LEO\x01\xE9" "RADIO");
    req.content_len = plen;
    req.recv_buf = (const char *)payload;
    req.recv_buf_len = plen;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    // 0x01 和 0xE9 被剔除,剩 "LEORADIO"
    CHECK(strcmp(s_slots[0].name, "LEORADIO") == 0);
}

// i3. dispname >32 字符 → 截断到 META_NAME_MAX(32)
static void test_dispname_truncate(void)
{
    TEST("i3_dispname_truncate");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.uri, sizeof(req.uri),
             "/api/upload?slot=0&dispname=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    req.content_len = plen;
    req.recv_buf = (const char *)payload;
    req.recv_buf_len = plen;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(strlen(s_slots[0].name) == META_NAME_MAX);
}

// i4. 无 dispname → esp_app_desc project_name 回退,blob 区保持 0xFF
static void test_no_dispname_fallback(void)
{
    TEST("i4_no_dispname_fallback");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    desc_inject_valid = true;
    snprintf(injected_desc.project_name, sizeof(injected_desc.project_name), "TestFirmware");
    snprintf(injected_desc.version, sizeof(injected_desc.version), "1.0.0");

    httpd_req_t req = make_upload_req(0, NULL, payload, plen);
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(strcmp(s_slots[0].name, "TestFirmware") == 0);
    CHECK(strcmp(s_slots[0].version, "1.0.0") == 0);

    // blob 区保持 0xFF(未写 dispname)
    const uint32_t blob_off = 0x1D6000 - 4096;
    bool all_ff = true;
    for (int i = 0; i < 64; i++) {
        if (ram_flash[0][blob_off + i] != 0xFF) { all_ff = false; break; }
    }
    CHECK(all_ff);
}

// i5. dispname URL 编码 'LEO%20RADIO' → 解码后 blob 得 "LEO RADIO"
static void test_dispname_url_decode(void)
{
    TEST("i5_dispname_url_decode");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.uri, sizeof(req.uri), "/api/upload?slot=0&dispname=LEO%%20RADIO");
    req.content_len = plen;
    req.recv_buf = (const char *)payload;
    req.recv_buf_len = plen;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(strcmp(s_slots[0].name, "LEO RADIO") == 0);

    const uint32_t blob_off = 0x1D6000 - 4096;
    char unpacked[META_NAME_MAX + 1];
    CHECK(meta_name_unpack(ram_flash[0] + blob_off, 64, unpacked, sizeof(unpacked)));
    CHECK(strcmp(unpacked, "LEO RADIO") == 0);
}

// i6. dispname 'A+B' → '+' 解码为空格 → "A B"
static void test_dispname_plus_decode(void)
{
    TEST("i6_dispname_plus_decode");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.uri, sizeof(req.uri), "/api/upload?slot=0&dispname=A+B");
    req.content_len = plen;
    req.recv_buf = (const char *)payload;
    req.recv_buf_len = plen;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(strcmp(s_slots[0].name, "A B") == 0);
}

// i7. dispname UTF-8 中文(%E4%B8%AD%E6%96%87)→ 解码后 >=0x80 被剔除为空,不写 blob
static void test_dispname_utf8_filtered(void)
{
    TEST("i7_dispname_utf8_filtered");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    // %E4%B8%AD%E6%96%87 = "中文" UTF-8,解码后字节 >=0x80,全被可打印过滤剔除
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.uri, sizeof(req.uri),
             "/api/upload?slot=0&dispname=%%E4%%B8%%AD%%E6%%96%%87");
    req.content_len = plen;
    req.recv_buf = (const char *)payload;
    req.recv_buf_len = plen;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    // dispname 为空 → 回退 esp_app_desc project_name
    CHECK(strcmp(s_slots[0].name, "unknown") == 0);

    // blob 区保持 0xFF(未写 dispname)
    const uint32_t blob_off = 0x1D6000 - 4096;
    bool all_ff = true;
    for (int i = 0; i < 64; i++) {
        if (ram_flash[0][blob_off + i] != 0xFF) { all_ff = false; break; }
    }
    CHECK(all_ff);
}

// i8. 非法 %XX 序列 '100%zz' → '%' 原样保留,可打印部分全保留
static void test_dispname_invalid_percent(void)
{
    TEST("i8_dispname_invalid_percent");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.uri, sizeof(req.uri), "/api/upload?slot=0&dispname=100%%zz");
    req.content_len = plen;
    req.recv_buf = (const char *)payload;
    req.recv_buf_len = plen;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(strcmp(s_slots[0].name, "100%zz") == 0);
}

// l. 总上传量 < 24B(10 字节,magic 正确都不够)→ 400 'upload broken' + INVALID
// ESP_ERR_INVALID_SIZE 路径,ota_end 不得被调用
static void test_total_less_than_header(void)
{
    TEST("l_total_less_than_header");
    setup();
    force_paired();
    uint8_t payload[10];
    memset(payload, 0xAB, sizeof(payload));
    payload[0] = 0xE9;  // magic 正确但总长不足 24B
    httpd_req_t req = make_upload_req(0, NULL, payload, sizeof(payload));
    h_upload(&req);
    CHECK(strcmp(req.resp_status, "400 Bad Request") == 0);
    CHECK(strcmp(req.resp_body, "upload broken") == 0);
    CHECK(ota_abort_count == 1);
    CHECK(ota_end_count == 0);   // ota_end 不得被调用
    CHECK(s_slots[0].state == META_SLOT_INVALID);
}

// j. 碎片化接收:每次 7 字节,内容仍逐字节正确
// 滚动头预检凑齐 24B 才校验,7 字节分块不再触发 TRUNCATED。
static void test_fragmented_recv(void)
{
    TEST("j_fragmented_recv");
    setup();
    force_paired();
    const size_t plen = 100;
    uint8_t payload[plen];
    make_payload(payload, plen);
    httpd_req_t req = make_upload_req(0, NULL, payload, plen);
    req.recv_chunk_size = 7;
    h_upload(&req);
    CHECK(strcmp(req.resp_body, "ok") == 0);
    CHECK(memcmp(ram_flash[0], payload, plen) == 0);
}

// k. 成功后第二次上传(未再配对)→ 403
static void test_single_use_session(void)
{
    TEST("k_single_use_session");
    setup();
    force_paired();
    const size_t plen = 256;
    uint8_t payload[plen];
    make_payload(payload, plen);

    httpd_req_t req1 = make_upload_req(0, NULL, payload, plen);
    h_upload(&req1);
    CHECK(strcmp(req1.resp_body, "ok") == 0);
    CHECK(!s_paired);

    httpd_req_t req2 = make_upload_req(0, NULL, payload, plen);
    h_upload(&req2);
    CHECK(strcmp(req2.resp_status, "403 Forbidden") == 0);
    CHECK(strcmp(req2.resp_body, "pair first") == 0);
}

int main(void)
{
    test_sha256_self();
    test_unpaired_403();
    test_session_pairing();
    test_slot_validation();
    test_content_length_limits();
    test_bad_magic();
    test_recv_failure();
    test_ota_write_failure();
    test_ota_end_failure();
    test_success_path();
    test_dispname_blob();
    test_dispname_filter();
    test_dispname_truncate();
    test_no_dispname_fallback();
    test_dispname_url_decode();
    test_dispname_plus_decode();
    test_dispname_utf8_filtered();
    test_dispname_invalid_percent();
    test_fragmented_recv();
    test_total_less_than_header();
    test_single_use_session();

    if (g_fails == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURES\n", g_fails);
    return 1;
}
