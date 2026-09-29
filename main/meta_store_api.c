// main/meta_store_api.c —— 实现见头文件注释。
// 两个 HTTP 端点共用同一 host 与 TLS 信任根;响应体按固定上限收进 static 缓冲,
// 不整包入堆。下载为"读一块 → SHA-256 更新 → esp_ota_write"单遍流式,
// 内存占用与块大小(1KB)无关固件大小。
#include "meta_store_api.h"
#include "meta_store_api_fail.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_image_format.h"   // esp_image_verify / esp_image_metadata_t
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_tls.h"            // r10.7:OPEN 死因分类(esp/mbedtls 错误码 +
                                // esp_tls_get_and_clear_error_type)
#include "lwip/errno.h"         // ECONNREFUSED/EHOSTUNREACH(SOCK errno 系)
#include "mbedtls/error.h"      // MBEDTLS_ERR_SSL_CONN_EOF
#include "mbedtls/sha256.h"
#include <time.h>               // time():时钟未同步判定(SNTP 前停在 1970)
#include <string.h>             // strerror():errno 死因串口输出

#include "meta_image.h"
#include "meta_name.h"
#include "meta_sign.h"
#include "meta_store.h"
#include "meta_store_analysis.h"
#include "meta_store_json.h"

static const char *TAG = "store_api";

// r10.7:OPEN 阶段底层死因捕获 —— esp_http_client 只回一个 ESP_FAIL,
// DNS 解析失败/连接超时/证书校验失败在屏上一个样(真机 r10.6 起实测)。
// 事件回调在作业任务上下文同步触发,读改无需锁;HTTP_EVENT_ERROR 的
// event->data 是 esp_tls_error_handle_t(IDF 5.x 契约)。
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ERROR || evt->data == NULL) return ESP_OK;
    const esp_tls_error_handle_t tls_err = (esp_tls_error_handle_t)evt->data;
    int code = 0;
    // 依序探测:esp 系(DNS/连接超时/拒连)→ mbedTLS 系(证书校验失败)。
    // 每层原始码都上串口(ESP_LOGE 在 WARN 默认级可见)—— 屏显死因之外,
    // 串口保留原始错误码供对照 IDF 错误表。
    if (esp_tls_get_and_clear_error_type(tls_err, ESP_TLS_ERR_TYPE_ESP, &code) == ESP_OK
        && code != 0) {
        ESP_LOGE(TAG, "OPEN esp-err=0x%x (%s)", code, esp_err_to_name(code));
        char raw[16];
        snprintf(raw, sizeof(raw), "esp=0x%x", code);
        meta_store_api_fail_set_raw(raw);
        if (code == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
            meta_store_api_fail_set_cause(MSAF_CAUSE_DNS);
        } else if (code == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
                   || code == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) {
            meta_store_api_fail_set_cause(MSAF_CAUSE_TIMEOUT);
        } else if (code == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) {
            meta_store_api_fail_set_cause(MSAF_CAUSE_REFUSED);
        }
        return ESP_OK;
    }
    if (esp_tls_get_and_clear_error_type(tls_err, ESP_TLS_ERR_TYPE_SYSTEM, &code) == ESP_OK
        && code != 0) {
        ESP_LOGE(TAG, "OPEN system-errno=%d (%s)", code, strerror(code));
        char raw[16];
        snprintf(raw, sizeof(raw), "sys=%d", code);
        meta_store_api_fail_set_raw(raw);
        // SOCK errno 系:ECONNREFUSED/EHOSTUNREACH 端口不通,DNS 失败在 lwip
        // 走 esp 系(0x8001),这里兜拒连/不可达。
        if (code == ECONNREFUSED || code == EHOSTUNREACH || code == ENETUNREACH) {
            meta_store_api_fail_set_cause(MSAF_CAUSE_REFUSED);
        }
        return ESP_OK;
    }
    // mbedTLS 系错误(证书校验失败落这里:X509 0x2700 系 / SSL 0x7700 系)。
    if (esp_tls_get_and_clear_error_type(tls_err, ESP_TLS_ERR_TYPE_MBEDTLS,
                                         &code) == ESP_OK && code != 0) {
        ESP_LOGE(TAG, "OPEN mbedtls-err=-0x%04x", code);
        char raw[16];
        snprintf(raw, sizeof(raw), "tls=-0x%04x", code);
        meta_store_api_fail_set_raw(raw);
        if ((code & 0xFF00) == 0x2700 || (code & 0xFF00) == 0x7700
            || code == MBEDTLS_ERR_SSL_CONN_EOF) {
            meta_store_api_fail_set_cause(MSAF_CAUSE_CERT);
        }
        return ESP_OK;
    }
    ESP_LOGW(TAG, "OPEN error event without capturable code");
    return ESP_OK;
}

// analyze 响应体上限:真实响应 ~600B,留足嵌套与数组余量;超限按格式错误处理。
#define ANALYZE_BUF_MAX  1536
// 下载/摘要分块:无 PSRAM,小堆常驻。
#define DL_CHUNK         1024
// 请求超时:构建期可用 CONFIG_META_STORE_HTTP_TIMEOUT_MS 覆盖(main/Kconfig.projbuild);
// host 桩编译无 sdkconfig,保留同值回退。
#ifdef CONFIG_META_STORE_HTTP_TIMEOUT_MS
#define HTTP_TIMEOUT_MS  CONFIG_META_STORE_HTTP_TIMEOUT_MS
#else
#define HTTP_TIMEOUT_MS  30000
#endif

// ---- 进度快照(网络任务写,UI 轮询读) ----

static meta_store_api_progress_t s_progress;
static volatile bool s_cancel;   // 下载取消请求(UI 写,网络任务读)

static void set_progress(bool active, int pct, uint32_t rx, uint32_t exp,
                         bool verify, const char *msg)
{
    s_progress.active = active;
    s_progress.progress_pct = pct;
    s_progress.received = rx;
    s_progress.expected = exp;
    s_progress.verify_phase = verify;
    snprintf(s_progress.message, sizeof(s_progress.message), "%s", msg);
}

void meta_store_api_poll(meta_store_api_progress_t *out)
{
    if (out) *out = s_progress;
}

void meta_store_api_request_cancel(void)
{
    s_cancel = true;
    // 立即把快照切到"取消中":进度条停止,屏上不再刷新百分比。
    if (s_progress.active) {
        set_progress(false, s_progress.progress_pct, s_progress.received,
                     s_progress.expected, false, "Cancelling...");
    }
}

// ---- analyze ----

// 分类文案的格式化缓冲(单作业任务串行调用,static 安全,48B 容纳全部文案)。
static char s_fail_buf[48];

// analyze 传输失败 → 分类文案(r10.2,BUG-12):此前一切传输失败都写死
// "unavailable",TLS/时钟/5xx/解析失败在屏上一个样,真机无从定位。
// 时钟判定:SDK time() 在 SNTP 未同步时停在 1970 附近 —— mbedTLS 证书
// 时间校验(notBefore/notAfter)会因此失败,这是真机"网络通了却 unavailable"
// 的最可能死因,必须显式点名。
static const char *analyze_fail_text(int code)
{
    const time_t now = time(NULL);
    const bool clock_unsynced = (now < 1700000000);   // ~2023-11 前视为未同步
    return meta_store_api_fail_text(code, clock_unsynced, s_fail_buf,
                                    sizeof(s_fail_buf));
}

esp_err_t meta_store_api_analyze(uint32_t play_id, meta_store_analysis_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    static char s_buf[ANALYZE_BUF_MAX];
    char url[96];
    snprintf(url, sizeof(url), "%s/api/analyze?id=%lu",
             META_STORE_API_BASE, (unsigned long)play_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        // 接收缓冲即头部解析缓冲:CF 边缘会发 set-cookie/report-to 等大头部,
        // 1KB 压线 → 解析截断风险;样例实现(真机验证)用 4096。
        .buffer_size = 4096,
        // r10.5 根因修复:此前从未配置任何信任锚 —— mbedTLS 没有可用的 CA,
        // 所有 HTTPS 握手必败,屏上只见 "TLS/DNS failed"(真机 v25 实测)。
        // crt_bundle_attach 指向 sdkconfig 自定义证书包(main/certs,双根)。
        .crt_bundle_attach = esp_crt_bundle_attach,
        // r10.7:OPEN 死因捕获(DNS/超时/拒连/证书),失败时屏显具体层。
        .event_handler = http_event_cb,
        // 商店请求的唯一 UA:服务端可观测设备流量;E2E 契约复演同源(meta_store_api.h)。
        .user_agent = META_STORE_API_USER_AGENT,
        // 禁自动重定向:301 响应本身携带 0 个 body 字节,被 IDF 静默跟随会
        // 让 fetch_headers() 返回下跳的 CL —— 长度契约在跨源时被污染。
        // 关掉后 301 变成显式错误上屏,域内路由策略变化不再静默。
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(out->reason, sizeof(out->reason), "%s", analyze_fail_text(MSAF_STAGE_OPEN));
        return ESP_FAIL;
    }

    int fail_stage = MSAF_STAGE_OPEN;   // 传输失败点(open→headers→read 递进)
    meta_store_api_fail_set_cause(MSAF_CAUSE_NONE);   // 本次作业死因归零
    meta_store_api_fail_set_raw("");                  // 原始码槽位同步清空
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        // r10.7:OPEN 死因已知时用具体文案,未知回落阶段句;串口同步留档。
        ESP_LOGE(TAG, "analyze open failed: %s", meta_store_api_fail_get_cause()
                 != MSAF_CAUSE_NONE ? "(cause captured above)" : "no cause captured");
        snprintf(out->reason, sizeof(out->reason), "%s",
                 meta_store_api_fail_open_text(time(NULL) < 1700000000,
                                               s_fail_buf, sizeof(s_fail_buf)));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const int64_t content_len = esp_http_client_fetch_headers(client);
    // r10.8(BUG-19):fetch_headers() 返回 Content-Length 而非状态码 ——
    // 旧代码把 ~600 当 "HTTP 600" 拒掉,即使 TLS 成功 analyze 也必败(IDF
    // esp_http_client.h:639 契约;真机宿敌)。状态码必须走 get_status_code()。
    const int status = esp_http_client_get_status_code(client);
    if (content_len < 0) {
        err = ESP_FAIL;
        fail_stage = MSAF_STAGE_HEADERS;
        ESP_LOGE(TAG, "analyze fetch_headers failed");
        goto fail;
    }
    if (status == 404) {
        ESP_LOGI(TAG, "analyze %lu: not-found", (unsigned long)play_id);
        snprintf(out->reason, sizeof(out->reason), "%s", "not-found");
        err = ESP_FAIL;
        goto fail_close;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "analyze HTTP %d", status);
        snprintf(out->reason, sizeof(out->reason), "%s", analyze_fail_text(-status));
        err = ESP_FAIL;
        goto fail_close;
    }
    // 301 明确上屏:禁了自动跟随,它不再被静默消化(域内路由策略变化可见)。
    if (content_len == 0) {   // 301/204 等无 body 响应:analyze 契约必失败
        ESP_LOGE(TAG, "analyze: no body (HTTP %d, redirect or empty?)", status);
        snprintf(out->reason, sizeof(out->reason), "%s", analyze_fail_text(MSAF_STAGE_PARSE));
        err = ESP_FAIL;
        goto fail_close;
    }

    int total = 0;
    for (;;) {
        const int got = esp_http_client_read(client, s_buf + total,
                                             ANALYZE_BUF_MAX - 1 - (size_t)total);
        if (got < 0) {
            err = ESP_FAIL;
            fail_stage = MSAF_STAGE_READ;
            ESP_LOGE(TAG, "analyze read failed");
            goto fail;
        }
        if (got == 0) break;
        total += got;
        if (total >= (int)ANALYZE_BUF_MAX - 1) {   // 响应超上限:契约外形态
            err = ESP_FAIL;
            goto fail_parse;
        }
    }
    s_buf[total] = '\0';
    ESP_LOGI(TAG, "analyze %lu: %d B", (unsigned long)play_id, total);
    if (!meta_store_analysis_parse(s_buf, (size_t)total, out)) {
        // parser 已置 out->reason="format"(契约缺失/字段非法),保留展示。
        ESP_LOGE(TAG, "analyze parse failed (%d B)", total);
        err = ESP_FAIL;
        goto fail_close;
    }
    esp_http_client_cleanup(client);
    return ESP_OK;

fail:
    // 传输失败(open/headers/read):reason = 分类文案(时钟×阶段)。
    snprintf(out->reason, sizeof(out->reason), "%s",
             analyze_fail_text(fail_stage));
fail_close:
    esp_http_client_cleanup(client);
    return err;

fail_parse:
    snprintf(out->reason, sizeof(out->reason), "%s", analyze_fail_text(MSAF_STAGE_PARSE));
    esp_http_client_cleanup(client);
    return ESP_FAIL;
}

// ---- install(流式下载刷槽) ----

// 十六进制小写化(64 字符 + '\0'),供槽位注册表与 HTTP 头比对共用。
static void hex64(const uint8_t digest[32], char out[META_SHA256_HEX_LEN + 1])
{
    static const char k_hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = k_hex[digest[i] >> 4];
        out[i * 2 + 1] = k_hex[digest[i] & 0x0F];
    }
    out[META_SHA256_HEX_LEN] = '\0';
}

// 把 analyze 里的显示名写入槽位尾 sector 的 MNAM 窗口(同 meta_net 的落盘纪律:
// 整 sector 擦除后只写 40B 窗口;签名为服务端签发场景不存在,未签名镜像统一走此路径)。
static void write_display_name(const esp_partition_t *part, uint32_t image_len,
                               const char *name)
{
    const uint32_t tail_off = meta_sign_sector_offset(image_len);
    if (tail_off + META_SIG_SECTOR > part->size || name[0] == '\0') return;
    uint8_t window[META_NAME_BLOB_RESERVE];
    memset(window, 0xFF, sizeof(window));
    if (meta_name_pack_tail(name, window, sizeof(window)) == 0) return;
    esp_err_t err = esp_partition_erase_range(part, tail_off, META_SIG_SECTOR);
    if (err == ESP_OK) {
        err = esp_partition_write(part, tail_off + META_NAME_BLOB_OFF,
                                  window, sizeof(window));
    }
    ESP_LOGI(TAG, "槽位显示名 %s: %s", (err == ESP_OK) ? "已写入" : "写入失败", name);
}

esp_err_t meta_store_api_install(uint32_t play_id, int slot,
                                 const meta_store_analysis_t *analysis,
                                 meta_slot_info_t slots[META_SLOT_COUNT])
{
    if (!analysis || !slots || slot < 0 || slot >= META_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *part = meta_store_slot_partition(slot);
    if (!part) return ESP_ERR_INVALID_ARG;

    // 双上限:analyze 声明的尺寸与槽位物理上限取小者;不一致即拒绝(防上游换版)。
    const uint32_t app_limit = meta_sign_app_limit(part->size);
    if (analysis->image_len > app_limit) {
        ESP_LOGE(TAG, "镜像 %lu 超过槽位 %d 上限 %lu",
                 (unsigned long)analysis->image_len, slot, (unsigned long)app_limit);
        return ESP_ERR_INVALID_SIZE;
    }

    char url[96];
    snprintf(url, sizeof(url), "%s/api/extracted?id=%lu",
             META_STORE_API_BASE, (unsigned long)play_id);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,                          // 同 analyze:头部解析缓冲防截断
        .crt_bundle_attach = esp_crt_bundle_attach,   // r10.5:同 analyze,信任锚缺失修复
        .user_agent = META_STORE_API_USER_AGENT,      // 同 analyze:设备 UA 唯一同源
        .disable_auto_redirect = true,                // 同 analyze:长度契约不跨源
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    set_progress(true, 0, 0, 0, false, "Connecting...");
    s_cancel = false;   // 清除可能在排队期间到达的取消请求,只响应当次下载
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) goto fail;

    // r10.8(BUG-19):与 analyze 同款修复 —— fetch_headers() 返回 CL 不是状态码;
    // 旧代码把 ~1.8MB 的 CL 当 "HTTP 1882272" 直接拒,install 在真机上从未成功过。
    const int64_t hdr_cl = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        err = (status < 0) ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
        ESP_LOGE(TAG, "install HTTP %d", status);
        goto fail;
    }
    if (hdr_cl < 0) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "install fetch_headers failed");
        goto fail;
    }

    // 同一 TLS 会话内的服务端权威声明:长度与摘要必须与 analyze 完全一致。
    // 拒绝 chunked/未知长度(content_len==-1):无声明长度就没有 TOCTOU 判据。
    const int64_t content_len = esp_http_client_get_content_length(client);
    if (content_len <= 0 || (uint64_t)content_len != (uint64_t)analysis->image_len) {
        ESP_LOGE(TAG, "长度声明不可用或不一致: header=%lld analyze=%lu",
                 (long long)content_len, (unsigned long)analysis->image_len);
        err = ESP_ERR_INVALID_SIZE;   // 上游已换版:要求重新 analyze
        goto fail;
    }
    char hdr_sha[META_SHA256_HEX_LEN + 1] = {0};
    // IDF 5.x 签名:成功时 *value 指向 client 内部缓冲(响应头生命周期内有效)。
    const char *hdr_value = NULL;
    if (esp_http_client_get_header(client, "x-image-sha256", (char **)&hdr_value) == ESP_OK
        && hdr_value != NULL && hdr_value[0] != '\0') {
        snprintf(hdr_sha, sizeof(hdr_sha), "%s", hdr_value);
        uint8_t hdr_digest[32];
        if (!meta_store_json_parse_sha256(hdr_sha, strlen(hdr_sha), hdr_digest)
            || memcmp(hdr_digest, analysis->sha256, 32) != 0) {
            ESP_LOGE(TAG, "摘要头与 analyze 不一致");
            err = ESP_ERR_INVALID_RESPONSE;
            goto fail;
        }
    }

    set_progress(true, 0, 0, (uint32_t)content_len, false, "Downloading...");

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(part, (size_t)content_len, &ota);
    if (err != ESP_OK) {
        // begin 内部会先擦目标区域:失败可能留下半擦除的槽位,必须作废注册表条目。
        esp_http_client_cleanup(client);
        meta_slot_mark_invalid(&slots[slot]);
        set_progress(false, -1, 0, 0, false, "Flash write failed.");
        return err;
    }

    static uint8_t s_chunk[DL_CHUNK];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    uint32_t received = 0;
    bool hdr_checked = false;
    uint8_t hdr[META_IMAGE_HEADER_LEN];
    size_t seen = 0;
    int pct = -1;

    for (;;) {
        if (s_cancel) {   // 用户取消:在当前块边界响应,半成品由下方统一作废
            err = ESP_FAIL;
            break;
        }
        const int got = esp_http_client_read(client, (char *)s_chunk, sizeof(s_chunk));
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) break;
        if (seen < META_IMAGE_HEADER_LEN) {
            const size_t take = ((size_t)got < META_IMAGE_HEADER_LEN - seen)
                              ? (size_t)got : META_IMAGE_HEADER_LEN - seen;
            memcpy(hdr + seen, s_chunk, take);
        }
        seen += (size_t)got;
        mbedtls_sha256_update(&sha, s_chunk, (size_t)got);
        err = esp_ota_write(ota, s_chunk, (size_t)got);
        if (err != ESP_OK) break;
        received += (uint32_t)got;
        const int p = (int)(received * 100u / (uint32_t)content_len);
        if (p != pct) {   // 进度变化才更新快照,避免无谓的跨任务写
            pct = p;
            set_progress(true, p, received, (uint32_t)content_len, false,
                         "Downloading...");
        }
        if (!hdr_checked && seen >= META_IMAGE_HEADER_LEN) {
            hdr_checked = true;
            if (meta_image_check_header(hdr, sizeof(hdr)) != META_IMG_OK) {
                ESP_LOGW(TAG, "镜像头预检失败");
                err = ESP_ERR_INVALID_ARG;
                break;
            }
        }
    }
    esp_http_client_cleanup(client);
    client = NULL;

    if (err != ESP_OK || !hdr_checked) {
        mbedtls_sha256_free(&sha);
        esp_ota_abort(ota);
        meta_slot_mark_invalid(&slots[slot]);
        const char *msg = s_cancel ? "Cancelled."
                         : (err == ESP_OK) ? "Truncated image."
                                           : "Download failed.";
        set_progress(false, -1, received, (uint32_t)content_len, false, msg);
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }

    // 流式摘要与服务端声明比对(信任链终点);不等 flash 回读——边下边算已覆盖全部字节。
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (received != (uint32_t)content_len) {   // r10.8:read() 已确认 EOF,残留字节即协议错
        ESP_LOGE(TAG, "字节流提前结束: %lu/%ld", (unsigned long)received, (long)content_len);
        esp_ota_abort(ota);
        meta_slot_mark_invalid(&slots[slot]);
        set_progress(false, -1, received, (uint32_t)content_len, false, "Truncated image.");
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(digest, analysis->sha256, 32) != 0) {
        ESP_LOGE(TAG, "流式 SHA-256 与 analyze 不一致");
        esp_ota_abort(ota);
        meta_slot_mark_invalid(&slots[slot]);
        set_progress(false, -1, received, (uint32_t)content_len, false,
                     "Checksum mismatch.");
        return ESP_ERR_INVALID_CRC;
    }

    set_progress(true, 100, received, (uint32_t)content_len, true, "Verifying...");
    if (esp_ota_end(ota) != ESP_OK) {   // IDF 权威校验:segment/校验和/尾部哈希
        meta_store_erase_slot(slot);
        meta_slot_mark_invalid(&slots[slot]);
        set_progress(false, -1, received, (uint32_t)content_len, false,
                     "Verify failed. Slot erased.");
        return ESP_ERR_INVALID_CRC;
    }

    // 权威获取 image_len(与 meta_store_scan 同一手法;不从 header 偏移猜)。
    esp_image_metadata_t meta = {0};
    const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
        meta_store_erase_slot(slot);
        meta_slot_mark_invalid(&slots[slot]);
        set_progress(false, -1, received, (uint32_t)content_len, false,
                     "Image verify failed.");
        return ESP_ERR_INVALID_CRC;
    }
    if (meta.image_len != analysis->image_len) {
        meta_store_erase_slot(slot);
        meta_slot_mark_invalid(&slots[slot]);
        set_progress(false, -1, received, (uint32_t)content_len, false,
                     "Image length mismatch.");
        return ESP_ERR_INVALID_SIZE;
    }

    char sha_hex[META_SHA256_HEX_LEN + 1];
    hex64(digest, sha_hex);
    esp_app_desc_t desc;
    const char *ver = "?";
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        ver = desc.version;
    }
    meta_slot_set_valid(&slots[slot], analysis->name, ver,
                        meta.image_len, sha_hex);
    write_display_name(part, meta.image_len, analysis->name);

    set_progress(false, 100, received, (uint32_t)content_len, false, "Installed.");
    ESP_LOGI(TAG, "槽位 %d 安装成功: %s (%lu B)", slot,
             analysis->name, (unsigned long)meta.image_len);
    return ESP_OK;

fail:
    // 走到这里时 esp_ota_begin 尚未成功:闪存未动,注册表保持原状——
    // 纯网络/校验错误不允许抹掉槽位里既有的有效固件信息。
    if (client) esp_http_client_cleanup(client);
    set_progress(false, -1, 0, 0, false,
                 err == ESP_FAIL ? "Network error." : "Version changed. Retry.");
    return err;
}
