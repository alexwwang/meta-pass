// main/meta_net.c —— 实现见头文件注释。资源预算遵循
// docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.zh_CN.md:
// AP-only、max_connection=1、1024B 分块、固定上限、进出页完整启停。
#include "meta_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

#include "meta_image.h"
#include "meta_name.h"
#include "meta_store.h"

static const char *TAG = "meta_net";

#define RX_CHUNK 1024   // 接收分块:无 PSRAM,禁止按整包申请内存

// ---- 模块状态(单实例;只有 httpd 任务写,UI 只读快照) ----
static mi_ctx_t         s_mi;                    // 导入状态机(纯逻辑,可测)
static meta_slot_info_t *s_slots;                // 启动器的槽位注册表(上传成功回写)
static httpd_handle_t   s_httpd;
static esp_netif_t     *s_ap_netif;
static bool             s_wifi_up;
static bool             s_nvs_ready;
static bool             s_netif_ready;
static bool             s_loop_ready;
static meta_net_status_t s_status;
static bool             s_paired;                // 单客户端会话:配对码已通过
static char             s_code[MI_CODE_LEN + 1]; // 本轮配对码

static void set_status(mi_state_t st, int pct, const char *msg)
{
    s_status.state = st;
    s_status.error = s_mi.last_error;
    s_status.progress_pct = pct;
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg);
}

// ---- NVS/netif/event loop 一次性准备(沿用 demo_radio 的纪律:NVS 失败绝不擦除) ----
static esp_err_t net_prepare(void)
{
    if (!s_nvs_ready) {
        esp_err_t err = nvs_flash_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除分区", esp_err_to_name(err));
            return err;
        }
        s_nvs_ready = true;
    }
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        s_netif_ready = true;
    }
    if (!s_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK) return err;
        s_loop_ready = true;
    }
    return ESP_OK;
}

// ---- 配对码与密码生成(物理持有 = 信任锚;字符集排除易混淆的 0/O/1/I/L) ----
static void gen_credentials(void)
{
    mi_code_gen(esp_random(), s_code);
    static const char k_set[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 8; i++) {
        s_status.password[i] = k_set[esp_random() % (sizeof(k_set) - 1)];
    }
    s_status.password[8] = '\0';
    snprintf(s_status.ssid, sizeof(s_status.ssid), "metapass-%04lX",
             (unsigned long)(esp_random() & 0xFFFF));
    snprintf(s_status.code, sizeof(s_status.code), "%s", s_code);
}

// ---- HTTP handlers ----

// 上传页:配对码 → 会话;选槽位 → 上传;XHR 显示进度。保持极简以省 RAM。
static const char INDEX_HTML[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>meta-pass import</title>"
    "<body style='font-family:sans-serif;max-width:28em;margin:2em auto'>"
    "<h3>meta-pass firmware import</h3>"
    "<p>1. Pairing code (shown on device screen):<br>"
    "<input id=c maxlength=6 size=8> <button onclick='pair()'>Pair</button> <b id=ps></b></p>"
    "<p>2. Slot: <select id=s><option value=0>Slot 0</option><option value=1>Slot 1</option>"
    "<option value=2>Slot 2</option></select></p>"
    "<p>3. Firmware (.bin app image):<br><input type=file id=f></p>"
    "<p>4. Display name (optional, shown in the device menu):<br>"
    "<input type=text id=dispname name=dispname maxlength=32 size=24"
    " placeholder='Display name (optional)'></p>"
    "<p><button onclick='up()'>Upload</button> <b id=st></b></p>"
    "<script>"
    "function pair(){fetch('/api/session?code='+c.value,{method:'POST'}).then(r=>{"
    "ps.textContent=r.ok?'paired':'wrong code ('+r.status+')';});}"
    "function up(){var x=new XMLHttpRequest();x.open('POST','/api/upload?slot='+s.value"
    "+'&dispname='+encodeURIComponent(dispname.value));"
    "x.upload.onprogress=e=>{if(e.lengthComputable)st.textContent="
    "' '+Math.round(100*e.loaded/e.total)+'%';};"
    "x.onload=()=>{st.textContent=' '+x.responseText;};"
    "x.send(f.files[0]);}"
    "</script>";

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_session(httpd_req_t *req)
{
    char arg[16] = {0};
    bool ok = false;
    if (httpd_req_get_url_query_str(req, arg, sizeof(arg)) == ESP_OK) {
        char code[MI_CODE_LEN + 2] = {0};
        if (httpd_query_key_value(arg, "code", code, sizeof(code)) == ESP_OK) {
            ok = mi_code_equal(code, s_code);
        }
    }
    mi_handle(&s_mi, ok ? MI_EV_PAIR_OK : MI_EV_PAIR_FAIL);
    s_paired = ok;
    if (s_mi.state == MI_ERROR) {
        set_status(s_mi.state, -1, "Too many wrong codes. Reopen Import on device.");
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "locked");
    }
    set_status(s_mi.state, -1, ok ? "Paired. Waiting for upload." : "Wrong code shown page.");
    if (!ok) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "wrong code");
    }
    return httpd_resp_sendstr(req, "paired");
}

// URL 查询值解码:ESP-IDF 的 httpd_query_key_value 不做 URL 解码
// (esp_http_server.h 注明 "components are not URLdecoded"),而页面侧
// dispname 经 encodeURIComponent 编码,必须在此解码(%XX 与 '+')。
// 非法 % 序列原样保留;就地解码,返回解码后长度。
static int url_hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char *s)
{
    size_t w = 0;
    for (size_t r = 0; s[r] != '\0';) {
        if (s[r] == '%') {
            const int hi = url_hex_nib(s[r + 1]);   // s[r+1] 至少是 '\0',安全
            const int lo = (hi >= 0) ? url_hex_nib(s[r + 2]) : -1;
            if (hi >= 0 && lo >= 0) {
                s[w++] = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        if (s[r] == '+') {
            s[w++] = ' ';
            r++;
            continue;
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

// 上传:Content-Length 上限 → 滚动头预检(凑齐 24B 即判)→ 流式写槽位 → esp_ota_end 权威校验。
// 任何失败都 esp_ota_abort 并把槽位标记 INVALID(残留半成品不可启动)。
static esp_err_t h_upload(httpd_req_t *req)
{
    if (!s_paired || s_mi.state != MI_PAIRED) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "pair first");
    }
    // query: slot=<n>&dispname=<form-urlencoded>(dispname 可选,最多 32 可打印 ASCII)
    char arg[200] = {0};
    int slot = -1;
    char disp[META_NAME_MAX + 1] = {0};
    if (httpd_req_get_url_query_str(req, arg, sizeof(arg)) == ESP_OK) {
        char sv[4] = {0};
        if (httpd_query_key_value(arg, "slot", sv, sizeof(sv)) == ESP_OK) {
            slot = (sv[0] >= '0' && sv[0] <= '9' && sv[1] == '\0') ? (sv[0] - '0') : -1;
        }
        // 显示名:URL 解码后剔除非可打印 ASCII,截断到 META_NAME_MAX;剔完为空则不写 blob
        char raw[160] = {0};
        if (httpd_query_key_value(arg, "dispname", raw, sizeof(raw)) == ESP_OK) {
            url_decode_inplace(raw);
            size_t w = 0;
            for (size_t r = 0; raw[r] != '\0' && w < META_NAME_MAX; r++) {
                const uint8_t b = (uint8_t)raw[r];
                if (b >= 0x20u && b <= 0x7Eu) disp[w++] = (char)b;
            }
            disp[w] = '\0';
        }
    }
    const esp_partition_t *part = meta_store_slot_partition(slot);
    if (!part) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad slot");
    }
    // 上限收紧:槽位尾部最后 4KB 保留给显示名 blob(按分区大小动态计算)。
    const uint32_t max_app = meta_name_max_app_size(part->size);
    if (!mi_content_length_ok(req->content_len, max_app)) {
        mi_handle(&s_mi, MI_EV_ABORT);
        s_mi.last_error = MI_ERR_TOO_LARGE;
        set_status(MI_ERROR, -1, "Firmware too large for slot.");
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "too large");
    }

    mi_handle(&s_mi, MI_EV_UPLOAD_BEGIN);
    set_status(MI_RECEIVING, 0, "Receiving...");

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, (size_t)req->content_len, &ota);
    if (err != ESP_OK) {
        mi_handle(&s_mi, MI_EV_ABORT);
        set_status(MI_ERROR, -1, "Flash write init failed.");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "ota begin failed");
    }

    uint8_t *buf = malloc(RX_CHUNK);
    if (!buf) {
        esp_ota_abort(ota);
        mi_handle(&s_mi, MI_EV_ABORT);
        set_status(MI_ERROR, -1, "Out of memory.");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no mem");
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    int remaining = req->content_len;
    esp_err_t result = ESP_OK;
    // 头预检按累计字节触发,不依赖首个分块尺寸:TCP 分片可能让首块不足 24B,
    // 边收边写 flash,前 24B 滚动留存,凑齐即校验,不合法立即中止(已擦除,
    // 至多白写 24B,abort 后无残留)。
    uint8_t hdr[META_IMAGE_HEADER_LEN];
    size_t  seen = 0;
    bool    hdr_checked = false;
    while (remaining > 0) {
        const int want = remaining < RX_CHUNK ? remaining : RX_CHUNK;
        const int got = httpd_req_recv(req, (char *)buf, want);
        if (got <= 0) {   // 0=对端关闭,<0=超时/错误:都按接收中断处理
            result = ESP_FAIL;
            break;
        }
        if (seen < META_IMAGE_HEADER_LEN) {
            const size_t take = ((size_t)got < META_IMAGE_HEADER_LEN - seen)
                              ? (size_t)got : META_IMAGE_HEADER_LEN - seen;
            memcpy(hdr + seen, buf, take);
        }
        seen += (size_t)got;
        err = esp_ota_write(ota, buf, (size_t)got);
        if (err != ESP_OK) {
            result = err;
            break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)got);
        remaining -= got;
        set_status(MI_RECEIVING, (req->content_len - remaining) * 100 / req->content_len,
                   "Receiving...");
        if (!hdr_checked && seen >= META_IMAGE_HEADER_LEN) {
            hdr_checked = true;
            const meta_img_err_t pre = meta_image_check_header(hdr, sizeof(hdr));
            if (pre != META_IMG_OK) {
                ESP_LOGW(TAG, "镜像预检失败: %s", meta_image_err_str(pre));
                result = ESP_ERR_INVALID_ARG;
                break;
            }
        }
    }
    if (result == ESP_OK && !hdr_checked) {
        result = ESP_ERR_INVALID_SIZE;   // 总长不足一个镜像头
    }
    free(buf);

    if (result != ESP_OK) {
        esp_ota_abort(ota);
        mbedtls_sha256_free(&sha);
        meta_slot_mark_invalid(&s_slots[slot]);
        mi_handle(&s_mi, MI_EV_ABORT);
        set_status(MI_ERROR, -1, "Upload broken. Slot invalidated.");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "upload broken");
    }

    mi_handle(&s_mi, MI_EV_UPLOAD_DONE);
    set_status(MI_VERIFYING, 100, "Verifying...");

    if (esp_ota_end(ota) != ESP_OK) {   // 权威校验:segment/校验和/尾部哈希
        mbedtls_sha256_free(&sha);
        meta_store_erase_slot(slot);
        meta_slot_mark_invalid(&s_slots[slot]);
        mi_handle(&s_mi, MI_EV_VERIFY_FAIL);
        set_status(MI_ERROR, -1, "Verify failed. Slot erased.");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "verify failed");
    }

    // 元数据:esp_app_desc 的名称/版本 + 流式 SHA-256(与上传方文件可人工比对)。
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    char sha_hex[META_SHA256_HEX_LEN + 1];
    for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", digest[i]);

    esp_app_desc_t desc;
    const char *name = "unknown";
    const char *ver = "?";
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        name = desc.project_name;
        ver = desc.version;
    }
    meta_slot_set_valid(&s_slots[slot], disp[0] != '\0' ? disp : name, ver,
                        (uint32_t)req->content_len, sha_hex);

    // 显示名 blob:擦除槽位尾部 4KB sector 后写入(偏移按分区大小动态计算)。
    // 擦/写失败仅记日志,不影响导入成功状态(扫描时无 blob 则回退 project_name)。
    if (disp[0] != '\0') {
        uint8_t blob[64];
        memset(blob, 0xFF, sizeof(blob));   // 未写字节保持擦除态
        const size_t blob_len = meta_name_pack(disp, blob, sizeof(blob));
        const size_t write_len = (blob_len + 3u) & ~(size_t)3u;   // esp_partition_write 4B 对齐
        const uint32_t blob_off = meta_name_blob_offset(part->size);
        esp_err_t berr = (blob_len > 0) ? ESP_OK : ESP_ERR_INVALID_ARG;
        if (berr == ESP_OK) {
            berr = esp_partition_erase_range(part, blob_off, META_NAME_BLOB_SECTOR);
        }
        if (berr == ESP_OK) {
            berr = esp_partition_write(part, blob_off, blob, write_len);
        }
        if (berr != ESP_OK) {
            ESP_LOGW(TAG, "槽位 %d 显示名 blob 写入失败: %s", slot, esp_err_to_name(berr));
        } else {
            ESP_LOGI(TAG, "槽位 %d 显示名: %s", slot, disp);
        }
    }

    s_paired = false;   // 一次配对一次上传,上传完即失效
    mi_handle(&s_mi, MI_EV_VERIFY_OK);
    set_status(MI_DONE, 100, "Done. You can boot it from the device menu.");
    ESP_LOGI(TAG, "槽位 %d 写入成功: %s %s (%d B)", slot, name, ver, req->content_len);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t h_status(httpd_req_t *req)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"state\":%d,\"error\":%d,\"progress\":%d}",
             s_status.state, s_status.error, s_status.progress_pct);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// ---- 启停 ----

esp_err_t meta_net_start(meta_slot_info_t slots[META_SLOT_COUNT])
{
    if (s_httpd) meta_net_stop();   // 幂等:先完整释放再重来
    if (!slots) return ESP_ERR_INVALID_ARG;
    s_slots = slots;
    mi_init(&s_mi);
    s_paired = false;
    gen_credentials();

    esp_err_t err = net_prepare();
    if (err != ESP_OK) goto fail;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) { err = ESP_ERR_NO_MEM; goto fail; }

    const wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);   // 不写 NVS 凭证
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) goto fail;
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 1,      // 单上传方
            .beacon_interval = 100,
        },
    };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_status.ssid);
    ap.ap.ssid_len = strlen(s_status.ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_status.password);
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_up = true;

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 6;
    hcfg.max_open_sockets = 3;        // 经验值:单客户端页面 + 上传
    hcfg.backlog_conn = 2;
    hcfg.lru_purge_enable = true;
    hcfg.stack_size = 6144;
    hcfg.recv_wait_timeout = 10;
    hcfg.send_wait_timeout = 10;
    err = httpd_start(&s_httpd, &hcfg);
    if (err != ESP_OK) goto fail;

    const httpd_uri_t uri_index   = { "/",           HTTP_GET,  h_index,   NULL };
    const httpd_uri_t uri_session = { "/api/session", HTTP_POST, h_session, NULL };
    const httpd_uri_t uri_upload  = { "/api/upload",  HTTP_POST, h_upload,  NULL };
    const httpd_uri_t uri_status  = { "/api/status",  HTTP_GET,  h_status,  NULL };
    httpd_register_uri_handler(s_httpd, &uri_index);
    httpd_register_uri_handler(s_httpd, &uri_session);
    httpd_register_uri_handler(s_httpd, &uri_upload);
    httpd_register_uri_handler(s_httpd, &uri_status);

    mi_handle(&s_mi, MI_EV_AP_READY);
    set_status(MI_AP_UP, -1, "AP ready. Pair from the web page.");
    ESP_LOGI(TAG, "SoftAP 就绪: %s(导入页 %s)", s_status.ssid, "http://192.168.4.1/");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "导入通道启动失败: %s", esp_err_to_name(err));
    set_status(MI_ERROR, -1, "Failed to start import mode.");
    s_mi.state = MI_ERROR;
    s_mi.last_error = MI_ERR_BROKEN;
    meta_net_stop();
    return err;
}

void meta_net_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_wifi_up) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_up = false;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    mi_init(&s_mi);
    set_status(MI_IDLE, -1, "");
    s_paired = false;
}

void meta_net_poll(meta_net_status_t *out)
{
    if (out) *out = s_status;
}
