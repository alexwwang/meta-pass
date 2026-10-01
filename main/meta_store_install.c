// main/meta_store_install.c —— LAN 手机辅助安装的设备侧通道(feat/mota v1)。
//
// 实现范围(设计文档 docs/assets/lan-pair-install-design.md):
//   §4.1 设备自托管最小 boot 页面(GET /,含非脚本回退与同源 DeviceBridge);
//   §6.5 本地 install HTTP 面(prepare/session/chunk/finalize/status/cancel/pair);
//   §8   一次性 128-bit token、配对码兜底(5 分钟 TTL / 5 次上限 / 一次性)、
//         全部本地 API 要求 X-Meta-Session、带 Origin 时必须等于设备 origin、
//         上传不先于物理槽位确认、失败路径槽位 INVALID。
// manifest 形状/边界、槽位几何复核、session/chunk 偏移判定在 meta_install_model
// (纯逻辑,host 测试同一份);本模块只做 HTTP/OTA/token 副作用与上屏快照。
//
// 线程模型:URI handler 全部运行在本地 httpd 任务内,状态读写在 handler 与
// UI 回调(确认/拒绝/取消)之间按"单写者 + 标志后置"约定交接:UI 提交的
// 确认先写 confirmed_slot 再置 confirmed 标志;结构快照拷贝(offer_copy/poll)
// 允许读到过渡态,最终一致性由 session 开启时的实时校验兜底。与 meta_store_net
// 的 httpd 纪律一致,UI 不直接碰阻塞网络调用。
//
// v1 信任模型(文档 §3/§12):设备不向 metapass 重查 analyze,只校验 manifest
// 形状/边界、上传长度/sha256、ESP 镜像结构;剩余保护 = 一次性 LAN token、
// 物理槽位确认、image_len/sha256/slot 强绑定、esp_ota_end/esp_image_verify、
// 失败后槽位 INVALID。签名 install manifest 是后续加固项。
#include "meta_store_install.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_netif.h>
#include <mbedtls/sha256.h>

#include "meta_store.h"
#include "meta_store_json.h"
#include "meta_name.h"
#include "meta_sign.h"

static const char *TAG = "install_local";

// ---- 常量 ----

// chunk 流式落盘的单次读缓冲(设备内部按 flash 写尺度拆块,§6.5)。
#define INSTALL_IO_BUF       4096
// prepare body 上限:manifest 是有界小 JSON(定长字段 + 3 槽位)。
#define INSTALL_PREPARE_MAX  1536
#define INSTALL_JSON_MAX     256

// ---- 状态(单例;v1 只允许一个 install 会话,§8) ----

typedef struct {
    bool     token_valid;
    char     token_hex[META_INSTALL_TOKEN_HEX_LEN + 1];
    char     pair_code[META_INSTALL_PAIR_DIGITS + 1];
    bool     pair_valid;         // 配对码未作废(未过期、未超次)
    bool     pair_used;          // 已被成功兑换(一次性)
    int64_t  pair_created_ms;    // 生成时刻(esp_timer 单调毫秒)
    int      pair_tries;         // 已累计错误次数
} session_token_t;

typedef struct {
    // ---- 快照字段(状态机唯一事实源;poll/status JSON 由此组装) ----
    bool     active;             // 店内 token 会话存活
    bool     offer_ready;        // 手机已 prepare,等待设备物理确认
    bool     confirmed;          // 已物理确认(上传可开始,§8)
    bool     session_opened;     // 手机已开上传 session
    int8_t   confirmed_slot;     // 物理确认的槽位(-1 = 无)
    uint32_t session_offset;     // 设备已写偏移(断点续传基准)
    char     name[META_NAME_LEN + 1];
    const char *state;           // pairing/offer/confirmed/uploading/done/failed/cancelled
    const char *message;

    // ---- offer 负载 ----
    meta_install_manifest_t manifest;
    bool     manifest_valid;

    // ---- 上传执行态 ----
    mbedtls_sha256_context sha;
    bool     sha_started;
    esp_ota_handle_t ota;
    bool     ota_open;
    bool     flash_touched;      // esp_ota_begin 成功过 = 旧内容已擦,失败必作废
} install_session_t;

static meta_slot_info_t *s_slots;           // 启动器槽位注册表(由 init 登记)
static httpd_handle_t    s_httpd;            // 本地 install httpd(端口 80,STA 模式)
static bool              s_init;             // init 完成
static session_token_t   s_token;
static install_session_t s_session;

// ---- 小工具 ----

static void status_set(const char *state, const char *msg)
{
    s_session.state = state;
    s_session.message = msg;
}

// 本地分区几何快照(注入 meta_install_model;分区不存在的槽位上限为 0)。
static void geom_refresh(meta_install_geom_t *g)
{
    memset(g, 0, sizeof(*g));
    if (!s_slots) return;
    for (int i = 0; i < META_SLOT_COUNT; i++) {
        const esp_partition_t *part = meta_store_slot_partition(i);
        if (part) g->limit[i] = meta_sign_app_limit(part->size);
    }
}

void meta_install_local_geom(meta_install_geom_t *out)
{
    if (out) geom_refresh(out);
}

// 清 offer 与上传残留(不改 state/message,由各终态自己给出文案)。
static void offer_and_upload_clear(void)
{
    if (s_session.ota_open) {
        esp_ota_abort(s_session.ota);
        s_session.ota_open = false;
    }
    if (s_session.sha_started) {
        mbedtls_sha256_free(&s_session.sha);
        s_session.sha_started = false;
    }
    memset(&s_session.manifest, 0, sizeof(s_session.manifest));
    s_session.manifest_valid = false;
    s_session.offer_ready = false;
    s_session.confirmed = false;
    s_session.confirmed_slot = -1;
    s_session.session_opened = false;
    s_session.session_offset = 0;
    s_session.flash_touched = false;
}

// 终态失败(文档 §6.5):中止 OTA;flash 被动过即槽位 INVALID;清 offer 手机侧重来。
static void fail_locked(const char *msg)
{
    const int8_t slot = s_session.confirmed_slot;
    const bool touched = s_session.flash_touched;
    offer_and_upload_clear();
    if (touched && slot >= 0 && s_slots) {
        meta_slot_mark_invalid(&s_slots[slot]);
        ESP_LOGW(TAG, "install failed; slot %d marked INVALID", slot);
    }
    status_set("failed", msg);
}

// ---- LAN 地址(实时查 esp_netif;配网层不需要回调注入) ----

uint32_t meta_install_lan_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return 0;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) return 0;
    return ip.ip.addr;   // 网络字节序;小端下内存序即 a.b.c.d
}

bool meta_install_lan_ready(void)
{
    return s_init && s_httpd != NULL && meta_install_lan_ip() != 0;
}

// ---- token / 配对码 / QR 信息 ----

static void token_to_hex(const uint8_t token[META_INSTALL_TOKEN_BYTES],
                         char out[META_INSTALL_TOKEN_HEX_LEN + 1])
{
    static const char k_hex[] = "0123456789abcdef";
    for (int i = 0; i < META_INSTALL_TOKEN_BYTES; i++) {
        out[i * 2]     = k_hex[token[i] >> 4];
        out[i * 2 + 1] = k_hex[token[i] & 0x0F];
    }
    out[META_INSTALL_TOKEN_HEX_LEN] = '\0';
}

// 从 IP 构造 QR 用的 http URL(v1 只考虑 IPv4,文档 §2)。
static void lan_url_into(char *out, size_t out_sz, const char *token_hex)
{
    const uint32_t ip = meta_install_lan_ip();
    if (ip == 0) {
        snprintf(out, out_sz, "http://0.0.0.0/#s=%s", token_hex);
        return;
    }
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, out_sz, "http://%u.%u.%u.%u/#s=%s",
             b[0], b[1], b[2], b[3], token_hex);
}

static esp_err_t token_generate(void)
{
    // 128 bit 随机(esp_random 每次 32 bit,拼 4 次;文档 §8 下限)。
    // token 只以 hex 形态驻留(比较也走 hex),不单独存字节副本。
    uint8_t raw[META_INSTALL_TOKEN_BYTES];
    for (int i = 0; i < META_INSTALL_TOKEN_BYTES; i++) {
        raw[i] = (uint8_t)esp_random();
    }
    token_to_hex(raw, s_token.token_hex);
    s_token.token_valid = true;
    snprintf(s_token.pair_code, sizeof(s_token.pair_code), "%06u",
             (unsigned)(esp_random() % 1000000U));
    s_token.pair_code[META_INSTALL_PAIR_DIGITS] = '\0';
    s_token.pair_valid = true;
    s_token.pair_used = false;
    s_token.pair_tries = 0;
    s_token.pair_created_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

void meta_install_qr_info(const char **token_hex_out, const char **pair_code_out,
                          char url_buf[256])
{
    if (token_hex_out) {
        *token_hex_out = s_token.token_valid ? s_token.token_hex : NULL;
    }
    if (pair_code_out) {
        *pair_code_out = (s_token.pair_valid && !s_token.pair_used)
                             ? s_token.pair_code : NULL;
    }
    if (url_buf) {
        lan_url_into(url_buf, 256, s_token.token_valid ? s_token.token_hex : "");
    }
}

esp_err_t meta_install_token_start(void)
{
    if (!s_init) return ESP_ERR_INVALID_STATE;
    if (!s_token.token_valid) {
        token_generate();
    }
    s_session.active = true;
    // 无在途 offer/session 时归位 pairing(首次进店、失败/取消/拒绝回退);
    // 有在途 offer 则保持现状,不覆盖手机侧流程。
    if (!s_session.offer_ready && !s_session.confirmed && !s_session.session_opened) {
        status_set("pairing", "scan QR or enter pair code");
    }
    return ESP_OK;
}

void meta_install_token_stop(void)
{
    // 离店:在途会话终止;flash 被动过即槽位 INVALID(§8),token/配对码全作废。
    if (s_session.flash_touched && s_session.confirmed_slot >= 0 && s_slots) {
        meta_slot_mark_invalid(&s_slots[s_session.confirmed_slot]);
    }
    offer_and_upload_clear();
    memset(&s_token, 0, sizeof(s_token));
    s_session.active = false;
    status_set("idle", "");
}

bool meta_install_token_from_hex(const char *hex, size_t hex_len)
{
    if (!hex || hex_len != META_INSTALL_TOKEN_HEX_LEN) return false;
    if (!s_token.token_valid) return false;
    // 先验字符集(只接受小写 hex),再与签发 token 定长比较。
    for (int i = 0; i < META_INSTALL_TOKEN_HEX_LEN; i++) {
        const char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return memcmp(hex, s_token.token_hex, META_INSTALL_TOKEN_HEX_LEN) == 0;
}

// 配对码兑换(§8:短 TTL + 尝试上限 + 一次性)。
static bool pair_redeem(const char *code, size_t code_len)
{
    if (!s_token.token_valid || !s_token.pair_valid || s_token.pair_used) {
        return false;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_token.pair_created_ms > (int64_t)META_INSTALL_PAIR_TTL_MS) {
        s_token.pair_valid = false;
        return false;
    }
    if (code_len != META_INSTALL_PAIR_DIGITS ||
        strncmp(code, s_token.pair_code, META_INSTALL_PAIR_DIGITS) != 0) {
        if (++s_token.pair_tries >= META_INSTALL_PAIR_MAX_TRIES) {
            s_token.pair_valid = false;   // 到达上限即作废(重新进店才再生成)
            ESP_LOGW(TAG, "pair code invalidated after %d tries", s_token.pair_tries);
        }
        return false;
    }
    s_token.pair_used = true;
    return true;
}

// ---- offer 状态机(文档 §6.4) ----

bool meta_install_offer_copy(meta_install_manifest_t *out)
{
    if (!out) return false;
    if (!s_session.manifest_valid || !s_session.offer_ready) return false;
    if (s_session.confirmed) return false;   // 只有待确认 offer 对 UI 有意义
    *out = s_session.manifest;
    return true;
}

void meta_install_session_poll(meta_install_session_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->active         = s_session.active;
    out->offer_ready    = s_session.offer_ready;
    out->confirmed      = s_session.confirmed;
    out->session_opened = s_session.session_opened;
    out->slot           = s_session.confirmed_slot;
    out->offset         = s_session.session_offset;
    // 与 /api/install/status 的 "expected" 字段同一事实源(确认前显示 0)。
    out->expected       = s_session.manifest_valid ? s_session.manifest.image_len : 0u;
    memcpy(out->name, s_session.name, sizeof(out->name));
    out->state   = s_session.state;
    out->message = s_session.message;
}

esp_err_t meta_install_confirm_slot(int8_t slot)
{
    if (!s_init || !s_session.manifest_valid || !s_session.offer_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session.confirmed) return ESP_ERR_INVALID_STATE;
    meta_install_geom_t g;
    geom_refresh(&g);
    // 只要求本地几何可装(文档 §6.4:用户可选任意本地 fit 槽位)。
    if (!meta_install_model_slot_fit(&g, slot, s_session.manifest.image_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_session.confirmed_slot = slot;   // 先写槽位,后置标志(读侧以标志为序)
    s_session.confirmed = true;
    status_set("confirmed", "waiting for phone upload");
    ESP_LOGI(TAG, "slot %d confirmed for %s (%u bytes)", slot, s_session.name,
             s_session.manifest.image_len);
    return ESP_OK;
}

esp_err_t meta_install_offer_reject(void)
{
    if (!s_session.manifest_valid || !s_session.offer_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session.confirmed || s_session.session_opened) {
        return ESP_ERR_INVALID_STATE;   // 已确认的走 cancel,不走拒绝
    }
    offer_and_upload_clear();
    s_session.name[0] = '\0';
    status_set("pairing", "offer declined on device");
    return ESP_OK;
}

// ---- 手机侧动作(由 HTTP handler 调用) ----

esp_err_t meta_install_session_open(const meta_install_session_req_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (!s_session.manifest_valid || !s_session.offer_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_session.confirmed) return ESP_ERR_INVALID_STATE;      // 上传不得先于确认(§8)
    if (s_session.session_opened) return ESP_ERR_INVALID_STATE;  // 单 session(§8)

    meta_install_geom_t g;
    geom_refresh(&g);
    if (!meta_install_model_session_ok(&s_session.manifest, s_session.confirmed_slot,
                                       req, &g)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_session.session_opened = true;
    s_session.session_offset = 0;
    s_session.ota_open = false;
    s_session.flash_touched = false;
    mbedtls_sha256_init(&s_session.sha);
    mbedtls_sha256_starts(&s_session.sha, 0);
    s_session.sha_started = true;
    status_set("confirmed", "upload session ready");
    return ESP_OK;
}

esp_err_t meta_install_chunk_accept(uint32_t offset, uint32_t length, bool *duplicate)
{
    if (duplicate) *duplicate = false;
    const meta_chunk_verdict_t v = meta_install_model_chunk(
        s_session.session_opened,
        s_session.session_offset,
        s_session.manifest_valid ? s_session.manifest.image_len : 0,
        offset, length, META_INSTALL_MAX_CHUNK);
    if (v == META_CHUNK_OK) return ESP_OK;
    if (v == META_CHUNK_DUP) {
        if (duplicate) *duplicate = true;   // 幂等重复:整段已写过,跳过写(§6.5)
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t meta_install_chunk_write(const void *data, uint32_t length)
{
    if (!s_session.session_opened || !s_session.sha_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || length == 0) return ESP_ERR_INVALID_ARG;

    if (!s_session.ota_open) {
        const esp_partition_t *part = meta_store_slot_partition(s_session.confirmed_slot);
        if (!part) {
            fail_locked("partition missing");
            return ESP_ERR_INVALID_STATE;
        }
        // begin 即擦除旧内容:从这一刻起失败路径必须作废槽位。
        const esp_err_t err = esp_ota_begin(part, s_session.manifest.image_len,
                                            &s_session.ota);
        if (err != ESP_OK) {
            s_session.flash_touched = true;
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            fail_locked("flash begin failed");
            return err;
        }
        s_session.ota_open = true;
        s_session.flash_touched = true;
    }

    mbedtls_sha256_update(&s_session.sha, data, length);
    const esp_err_t err = esp_ota_write(s_session.ota, data, length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed at offset %u: %s",
                 s_session.session_offset, esp_err_to_name(err));
        fail_locked("flash write failed");
        return err;
    }

    s_session.session_offset += length;
    if (strcmp(s_session.state, "uploading") != 0) {
        status_set("uploading", "uploading");
    }
    return ESP_OK;
}

esp_err_t meta_install_finalize(void)
{
    if (!s_session.session_opened) return ESP_ERR_INVALID_STATE;

    const uint32_t expected = s_session.manifest_valid ? s_session.manifest.image_len : 0;
    if (!meta_install_model_finalize_ready(s_session.session_offset, expected)) {
        ESP_LOGE(TAG, "finalize: received %u != imageLen %u",
                 s_session.session_offset, expected);
        fail_locked("length mismatch");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&s_session.sha, digest);
    mbedtls_sha256_free(&s_session.sha);
    s_session.sha_started = false;

    if (memcmp(digest, s_session.manifest.sha256, 32) != 0) {
        ESP_LOGE(TAG, "finalize: app sha256 mismatch");
        fail_locked("checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    // esp_ota_end:OTA 正式提交 + 镜像校验(设备端权威校验点之一,§6.5)。
    if (!s_session.ota_open) {
        fail_locked("no ota session");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t end_err = esp_ota_end(s_session.ota);
    s_session.ota_open = false;
    if (end_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(end_err));
        fail_locked("verify failed");
        return ESP_ERR_INVALID_CRC;
    }

    // esp_image_verify(SILENT):权威复核 + 取 image_len 与 offer 比对(§6.5)。
    const int8_t slot = s_session.confirmed_slot;
    const esp_partition_t *part = meta_store_slot_partition(slot);
    if (!part) {
        fail_locked("partition missing");
        return ESP_ERR_INVALID_STATE;
    }
    esp_image_metadata_t meta = {0};
    const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
        ESP_LOGE(TAG, "image_verify failed on slot %d", slot);
        fail_locked("image verify failed");
        return ESP_ERR_INVALID_CRC;
    }
    if (meta.image_len != s_session.manifest.image_len) {
        ESP_LOGE(TAG, "image length mismatch: verify=%u manifest=%u",
                 meta.image_len, s_session.manifest.image_len);
        fail_locked("image length mismatch");
        return ESP_ERR_INVALID_SIZE;
    }

    // 展示名 + 注册表(先算好全部入参,避免终态后读已清字段)。
    char sha_hex[META_SHA256_HEX_LEN + 1];
    {
        static const char k_hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            sha_hex[i * 2]     = k_hex[digest[i] >> 4];
            sha_hex[i * 2 + 1] = k_hex[digest[i] & 0x0F];
        }
        sha_hex[META_SHA256_HEX_LEN] = '\0';
    }
    char name[META_NAME_LEN + 1];
    memcpy(name, s_session.name, sizeof(name));
    char ver[META_VERSION_LEN + 1] = "?";
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        memcpy(ver, desc.version, sizeof(ver) - 1);
        ver[sizeof(ver) - 1] = '\0';
    }

    if (!meta_slot_set_valid(&s_slots[slot], name, ver, meta.image_len, sha_hex)) {
        fail_locked("registry write failed");
        return ESP_ERR_INVALID_STATE;
    }

    // MNAM 显示名写入尾部 sector(与既有安装路径同一手法)。
    {
        const uint32_t tail_off = meta_sign_sector_offset(meta.image_len);
        if (tail_off + META_SIG_SECTOR <= part->size) {
            uint8_t window[META_NAME_BLOB_RESERVE];
            memset(window, 0xFF, sizeof(window));
            if (meta_name_pack_tail(name, window, sizeof(window)) != 0) {
                esp_err_t werr = esp_partition_erase_range(part, tail_off,
                                                           META_SIG_SECTOR);
                if (werr == ESP_OK) {
                    werr = esp_partition_write(part, tail_off + META_NAME_BLOB_OFF,
                                               window, sizeof(window));
                }
                ESP_LOGI(TAG, "MNAM write on slot %d: %s", slot,
                         (werr == ESP_OK) ? "ok" : "failed");
            }
        }
    }

    // 成功:清 offer 与上传态,保留 name/slot 供完成页展示;token 留到离店作废。
    offer_and_upload_clear();
    memcpy(s_session.name, name, sizeof(name));
    s_session.confirmed_slot = slot;
    status_set("done", "installed");
    ESP_LOGI(TAG, "LAN install slot %d done: %s (%u bytes)", slot, name,
             meta.image_len);
    return ESP_OK;
}

esp_err_t meta_install_cancel(void)
{
    // 取消(§6.5 status/cancel):中止 OTA;flash 被动过即槽位 INVALID。
    const int8_t slot = s_session.confirmed_slot;
    const bool touched = s_session.flash_touched;
    offer_and_upload_clear();
    if (touched && slot >= 0 && s_slots) {
        meta_slot_mark_invalid(&s_slots[slot]);
        ESP_LOGW(TAG, "install cancelled; slot %d marked INVALID", slot);
    }
    s_session.name[0] = '\0';
    status_set("cancelled", "cancelled");
    return ESP_OK;
}

int8_t meta_install_default_slot_from_manifest(const meta_install_manifest_t *m)
{
    if (!m) return -1;
    meta_install_geom_t g;
    geom_refresh(&g);
    return meta_install_model_default_slot(m, &g);
}

// ---- 本地 HTTP:公共小工具 ----

static bool origin_allowed(httpd_req_t *req)
{
    char origin[96];
    const esp_err_t e = httpd_req_get_hdr_value_str(req, "Origin", origin,
                                                    sizeof(origin));
    if (e == ESP_ERR_NOT_FOUND) return true;   // 非浏览器客户端:无 Origin
    if (e != ESP_OK) return false;             // 存在但超长:不可能等于设备 origin
    const uint32_t ip = meta_install_lan_ip();
    if (ip == 0) return false;                 // 地址未知时带 Origin 一律不放行
    const uint8_t *b = (const uint8_t *)&ip;
    char expect[32];
    snprintf(expect, sizeof(expect), "http://%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return strcmp(origin, expect) == 0;        // §8:必须与设备 origin 完全一致
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, msg);
}

static bool req_token_ok(httpd_req_t *req)
{
    char val[META_INSTALL_TOKEN_HEX_LEN + 2];
    if (httpd_req_get_hdr_value_str(req, META_INSTALL_SESSION_HDR, val,
                                    sizeof(val)) != ESP_OK) {
        return false;   // 缺头 / 超长都不算合法 token
    }
    return meta_install_token_from_hex(val, strlen(val));
}

// 读满请求体进调用方静态缓冲(httpd 任务内串行,静态缓冲安全)。
static esp_err_t req_body(httpd_req_t *req, char *buf, size_t cap, size_t *out_len)
{
    const size_t need = req->content_len;
    if (need == 0) return ESP_ERR_INVALID_ARG;
    if (need + 1 > cap) return ESP_ERR_INVALID_SIZE;   // 调用方映射 413
    size_t total = 0;
    while (total < need) {
        const int got = httpd_req_recv(req, buf + total, need - total);
        if (got <= 0) return ESP_FAIL;                 // 超时 / 连接断
        total += (size_t)got;
    }
    buf[total] = '\0';
    *out_len = total;
    return ESP_OK;
}

// 十进制 u32 解析(X-Meta-Offset):拒绝空串、负号、尾随垃圾。
static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || *s == '\0' || *s == '-') return false;
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

// JSON 字符串转义(仅 name 上屏字段可能含引号/反斜杠;其余为字面量)。
static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t w = 0;
    for (; *in != '\0' && w + 2 < out_sz; in++) {
        const char c = *in;
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = c;
        } else if ((unsigned char)c >= 0x20u) {
            out[w++] = c;
        }
    }
    out[w] = '\0';
}

// ---- 本地 HTTP:boot 页面(文档 §4.1) ----

// 极简壳,不是产品 UI。占位符恰好 4 个 %s(ip / state 各二,顺序勿动),
// 模板内禁止出现其他 '%'。页面职责:非脚本回退信息、同源 DeviceBridge
// (status/prepare/session/chunk/finalize/cancel)、配对码兑换入口。
// token 只来自 URL fragment 或 pair 响应,绝不进 HTML 响应体 —— 任何无 token
// 的 GET 都拿不到它。
static const char SHELL_HTML[] =
"<!doctype html><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>meta-pass install</title>"
"<style>body{font-family:sans-serif;max-width:24em;margin:2em auto}"
"#st{color:#555}#ps{color:#a00}</style>"
"<h3>meta-pass install</h3>"
"<div id=st>device %s &middot; session %s</div>"
"<noscript><p>JavaScript is off. Device %s, session %s. "
"Enable JavaScript and reload this page.</p></noscript>"
"<div id=pair>Pairing code: <input id=pc maxlength=6 inputmode=numeric "
"autocomplete=one-time-code> <button onclick=doPair()>Connect</button> "
"<span id=ps></span></div>"
"<script>"
"var T=new URLSearchParams(location.hash.slice(1)).get('s')||'';"
"function bridge(path,opt){opt=opt||{};var h={};"
"var oh=opt.headers||{};Object.keys(oh).forEach(function(k){h[k]=oh[k];});"
"if(T)h['X-Meta-Session']=T;"
"if(opt.json!==undefined){h['Content-Type']='application/json';"
"opt.body=JSON.stringify(opt.json);}"
"return fetch(path,opt).then(function(r){return r.text().then(function(t){"
"return {ok:r.ok,status:r.status,text:t};});});}"
"window.DeviceBridge={"
"token:function(){return T;},"
"status:function(){return bridge('/api/install/status');},"
"prepare:function(o){return bridge('/api/install/prepare',{method:'POST',json:o});},"
"session:function(o){return bridge('/api/install/session',{method:'POST',json:o});},"
"chunk:function(off,buf){return bridge('/api/install/chunk',{method:'POST',"
"headers:{'X-Meta-Offset':String(off),'Content-Type':'application/octet-stream'},"
"body:buf});},"
"finalize:function(){return bridge('/api/install/finalize',{method:'POST'});},"
"cancel:function(){return bridge('/api/install/cancel',{method:'POST'});}};"
"function doPair(){ps.textContent='...';"
"fetch('/api/install/pair',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({code:pc.value})})"
".then(function(r){return r.text().then(function(t){return [r.ok,t];});})"
".then(function(a){if(a[0]){T=JSON.parse(a[1]).token;location.hash='s='+T;"
"ps.textContent='paired';}else ps.textContent='pair rejected';})"
".catch(function(){ps.textContent='pair failed';});}"
"</script>"
"<script type=module "
"src=https://metapass.chuanxilu.net/phone-install.js></script>";

static esp_err_t h_boot_index(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    char ip[16] = "0.0.0.0";
    const uint32_t addr = meta_install_lan_ip();
    if (addr != 0) {
        const uint8_t *b = (const uint8_t *)&addr;
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    const char *state = s_session.state ? s_session.state : "idle";
    static char page[sizeof(SHELL_HTML) + 64];
    snprintf(page, sizeof(page), SHELL_HTML, ip, state, ip, state);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// ---- 本地 HTTP:install API(文档 §6.5) ----

// POST /api/install/pair —— 配对码换 token(§8 文本兜底;一次性)。
static esp_err_t h_install_pair(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    static char body[96];
    size_t len = 0;
    if (req_body(req, body, sizeof(body), &len) != ESP_OK) {
        return reply(req, "400 Bad Request", "bad body");
    }
    char code[META_INSTALL_PAIR_DIGITS + 2] = {0};
    if (!meta_store_json_get_string(body, len, "code", code, sizeof(code)) ||
        !pair_redeem(code, strlen(code))) {
        return reply(req, "403 Forbidden", "pair rejected");
    }
    char out[META_INSTALL_TOKEN_HEX_LEN + 32];
    snprintf(out, sizeof(out), "{\"token\":\"%s\"}", s_token.token_hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

// GET /api/install/status —— loader 兼容性握手(protocol)与轮询快照。
static esp_err_t h_install_status(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");

    char name_esc[META_NAME_LEN * 2 + 1];
    json_escape(s_session.name, name_esc, sizeof(name_esc));
    char body[384];
    const int n = snprintf(
        body, sizeof(body),
        "{\"protocol\":%d,\"state\":\"%s\",\"message\":\"%s\","
        "\"active\":%s,\"offer\":%s,\"confirmed\":%s,\"session\":%s,"
        "\"slot\":%d,\"offset\":%" PRIu32 ",\"expected\":%" PRIu32 ",\"name\":\"%s\"}",
        META_INSTALL_PROTOCOL_V1,
        s_session.state ? s_session.state : "idle",
        s_session.message ? s_session.message : "",
        s_session.active ? "true" : "false",
        s_session.offer_ready ? "true" : "false",
        s_session.confirmed ? "true" : "false",
        s_session.session_opened ? "true" : "false",
        s_session.confirmed_slot,
        s_session.session_offset,
        s_session.manifest_valid ? s_session.manifest.image_len : 0u,
        name_esc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, (size_t)n);
}

// POST /api/install/prepare —— 手机提交 offer(§6.4);物理确认前可被新 offer 覆盖。
static esp_err_t h_install_prepare(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (s_session.confirmed || s_session.session_opened) {
        return reply(req, "409 Conflict", "already confirmed");
    }

    static char body[INSTALL_PREPARE_MAX];
    size_t len = 0;
    const esp_err_t rd = req_body(req, body, sizeof(body), &len);
    if (rd == ESP_ERR_INVALID_SIZE) {
        return reply(req, "413 Payload Too Large", "manifest too large");
    }
    if (rd != ESP_OK) return reply(req, "400 Bad Request", "read error");

    meta_install_manifest_t m;
    if (!meta_install_model_parse(body, len, &m)) {
        ESP_LOGW(TAG, "prepare: manifest shape/bounds rejected");
        return reply(req, "400 Bad Request", "manifest rejected");
    }
    meta_install_geom_t g;
    geom_refresh(&g);
    if (!meta_install_model_offer_ok(&m, &g)) {
        ESP_LOGW(TAG, "prepare: local geometry re-check rejected");
        return reply(req, "400 Bad Request", "manifest rejected");
    }

    offer_and_upload_clear();          // 覆盖旧 offer 时清残留(未确认路径)
    s_session.manifest = m;
    s_session.manifest_valid = true;
    s_session.offer_ready = true;
    memcpy(s_session.name, m.name, sizeof(s_session.name));
    status_set("offer", "confirm on device");
    ESP_LOGI(TAG, "offer ready: %s (%u bytes)", m.name, m.image_len);
    return reply(req, "200 OK", "ok");
}

// POST /api/install/session —— 设备物理确认后开启上传 session(§6.5)。
static esp_err_t h_install_session(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (!s_session.confirmed) return reply(req, "409 Conflict", "not confirmed");
    if (s_session.session_opened) return reply(req, "409 Conflict", "session already open");

    static char body[INSTALL_JSON_MAX];
    size_t len = 0;
    const esp_err_t rd = req_body(req, body, sizeof(body), &len);
    if (rd == ESP_ERR_INVALID_SIZE) {
        return reply(req, "413 Payload Too Large", "session body too large");
    }
    if (rd != ESP_OK) return reply(req, "400 Bad Request", "read error");

    meta_install_session_req_t sreq;
    if (!meta_install_model_parse_session_req(body, len, &sreq)) {
        return reply(req, "400 Bad Request", "session rejected");
    }
    if (meta_install_session_open(&sreq) != ESP_OK) {
        ESP_LOGW(TAG, "session rejected: fields differ from confirmed offer");
        return reply(req, "400 Bad Request", "session rejected");
    }

    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"ready\",\"offset\":%" PRIu32 ",\"maxChunk\":%u}",
             s_session.session_offset, META_INSTALL_MAX_CHUNK);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

// POST /api/install/chunk —— 顺序分块写(§6.5:偏移来自 X-Meta-Offset 头)。
static esp_err_t h_install_chunk(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (!s_session.confirmed || !s_session.session_opened) {
        return reply(req, "409 Conflict", "not confirmed");
    }

    char off_str[16];
    uint32_t offset = 0;
    if (httpd_req_get_hdr_value_str(req, META_INSTALL_OFFSET_HDR, off_str,
                                    sizeof(off_str)) != ESP_OK ||
        !parse_u32(off_str, &offset)) {
        return reply(req, "400 Bad Request", "missing/bad X-Meta-Offset");
    }
    const uint32_t length = (uint32_t)req->content_len;
    if (length == 0 || length > META_INSTALL_MAX_CHUNK) {
        return reply(req, "400 Bad Request", "chunk length out of range");
    }

    bool dup = false;
    if (meta_install_chunk_accept(offset, length, &dup) != ESP_OK) {
        ESP_LOGW(TAG, "chunk rejected: offset %u len %u (written %u)",
                 offset, length, s_session.session_offset);
        return reply(req, "400 Bad Request", "chunk rejected");
    }

    static uint8_t io_buf[INSTALL_IO_BUF];
    size_t got_total = 0;
    if (dup) {
        // 幂等重复:读完丢弃,不写(§6.5 duplicate offsets are idempotent)。
        while (got_total < length) {
            const int got = httpd_req_recv(req, (char *)io_buf,
                                           length - got_total > INSTALL_IO_BUF
                                               ? INSTALL_IO_BUF
                                               : length - got_total);
            if (got <= 0) return reply(req, "400 Bad Request", "chunk read error");
            got_total += (size_t)got;
        }
        return reply(req, "200 OK", "ok");
    }

    // 流式写入:按 flash 写尺度拆块,不在 RAM 里攒整块(§6.5)。
    while (got_total < length) {
        const uint32_t want = (length - got_total > INSTALL_IO_BUF)
                                  ? INSTALL_IO_BUF : (length - got_total);
        const int got = httpd_req_recv(req, (char *)io_buf, want);
        if (got <= 0) {
            // 半块已写:offset 停在实际落盘处,手机按 status.offset 续传。
            return reply(req, "400 Bad Request", "chunk read error");
        }
        if (meta_install_chunk_write(io_buf, (uint32_t)got) != ESP_OK) {
            return reply(req, "500 Internal Server Error", "chunk write failed");
        }
        got_total += (size_t)got;
    }
    return reply(req, "200 OK", "ok");
}

// POST /api/install/finalize —— 长度/哈希/OTA/镜像/名称/注册表六连检(§6.5)。
static esp_err_t h_install_finalize(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    if (!s_session.confirmed || !s_session.session_opened) {
        return reply(req, "409 Conflict", "not confirmed");
    }
    if (meta_install_finalize() != ESP_OK) {
        return reply(req, "500 Internal Server Error", "finalize failed");
    }
    return reply(req, "200 OK", "ok");
}

// POST /api/install/cancel —— 手机侧取消(设备 UI 取消也走同一内部函数)。
static esp_err_t h_install_cancel(httpd_req_t *req)
{
    if (!origin_allowed(req)) return reply(req, "403 Forbidden", "origin not allowed");
    if (!req_token_ok(req)) return reply(req, "401 Unauthorized", "bad session token");
    meta_install_cancel();
    return reply(req, "200 OK", "ok");
}

// ---- 服务生命周期 ----

esp_err_t meta_install_net_init(meta_slot_info_t slots[META_SLOT_COUNT])
{
    if (!slots) return ESP_ERR_INVALID_ARG;
    if (s_init) return ESP_OK;
    s_slots = slots;
    memset(&s_session, 0, sizeof(s_session));
    s_session.confirmed_slot = -1;
    status_set("idle", "");
    s_init = true;
    return ESP_OK;
}

esp_err_t meta_install_net_start(void)
{
    if (!s_init) return ESP_ERR_INVALID_STATE;
    if (s_httpd) return ESP_OK;   // 幂等:已在跑

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 8;    // / + pair/status/prepare/session/chunk/finalize/cancel
    hcfg.max_open_sockets = 3;
    hcfg.backlog_conn = 2;
    hcfg.lru_purge_enable = true;
    hcfg.stack_size = 4096;
    hcfg.recv_wait_timeout = 10;
    hcfg.send_wait_timeout = 10;

    const esp_err_t err = httpd_start(&s_httpd, &hcfg);
    if (err != ESP_OK) {
        s_httpd = NULL;
        ESP_LOGE(TAG, "install httpd start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { "/",                    HTTP_GET,  h_boot_index,     NULL },
        { "/api/install/pair",    HTTP_POST, h_install_pair,    NULL },
        { "/api/install/status",  HTTP_GET,  h_install_status,  NULL },
        { "/api/install/prepare", HTTP_POST, h_install_prepare, NULL },
        { "/api/install/session", HTTP_POST, h_install_session, NULL },
        { "/api/install/chunk",   HTTP_POST, h_install_chunk,   NULL },
        { "/api/install/finalize",HTTP_POST, h_install_finalize,NULL },
        { "/api/install/cancel",  HTTP_POST, h_install_cancel,  NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_httpd, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed", uris[i].uri);
            httpd_stop(s_httpd);
            s_httpd = NULL;
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "LAN install service ready");
    return ESP_OK;
}

void meta_install_net_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    meta_install_token_stop();   // 幂等:未开 token 时也只是清空状态
}
