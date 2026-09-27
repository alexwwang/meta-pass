// main/meta_store_net.c —— 实现见头文件注释。
// 事件流:begin() → [有凭证] STA 连接等 IP_EVENT_STA_GOT_IP → SNTP 同步 → ONLINE;
//         begin() → [无凭证/连接失败] SoftAP + httpd 表单 → POST /api/wifi →
//         停 AP/启 STA(同上)。作业经队列进网络任务执行。
#include "meta_store_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "store_net";

#define AP_MAX_CONN        1
#define STA_CONNECT_MS     30000
#define SNTP_SYNC_MS       15000
#define JOB_QUEUE_LEN      2
#define JOB_STACK          6144
#define JOB_PRIO           5

// 商店会话超时:构建期默认(Kconfig),运行时可覆盖(见头文件注释);
// host 桩编译无 sdkconfig,保留同值回退。
#ifndef CONFIG_META_STORE_SESSION_TIMEOUT_MS
#define CONFIG_META_STORE_SESSION_TIMEOUT_MS (5 * 60 * 1000)
#endif
#define SESSION_TIMEOUT_MIN_MS   30000u
#define SESSION_TIMEOUT_MAX_MS   86400000u

static uint32_t s_session_timeout_ms = CONFIG_META_STORE_SESSION_TIMEOUT_MS;

void meta_store_session_set_timeout_ms(uint32_t ms)
{
    if (ms < SESSION_TIMEOUT_MIN_MS) ms = SESSION_TIMEOUT_MIN_MS;
    if (ms > SESSION_TIMEOUT_MAX_MS) ms = SESSION_TIMEOUT_MAX_MS;
    s_session_timeout_ms = ms;
}

uint32_t meta_store_session_timeout_ms(void)
{
    return s_session_timeout_ms;
}

// ---- 模块状态(网络任务写,UI 只读快照;与 meta_net 同一纪律) ----

static meta_slot_info_t  *s_slots;          // 启动器槽位注册表(install 成功回写)
static httpd_handle_t     s_httpd;
static esp_netif_t       *s_netif;          // 当前生效的 wifi netif(AP 或 STA)
static bool               s_wifi_up;
static bool               s_initialized;    // init() 完成(任务/队列/event handler 在)
static TaskHandle_t       s_job_task;
static QueueHandle_t      s_job_queue;
static EventGroupHandle_t s_events;
static meta_store_net_status_t s_status;
static meta_store_net_job_t    s_job;
static meta_store_analysis_t   s_analysis;  // 最近一次 analyze 成功结果
static bool               s_analysis_valid;

// s_events 位
#define EV_GOT_IP      BIT0
#define EV_DISCONNECT  BIT1
#define EV_CREDENTIALS BIT2   // 配网页已收到凭证(内容在 s_prov_*)
#define EV_STOP        BIT3

static char s_prov_ssid[33];
static char s_prov_pass[65];

static void set_state(sn_state_t st, const char *msg)
{
    s_status.state = st;
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg);
}

static void set_job(sn_job_state_t st, const char *msg)
{
    s_job.state = st;
    snprintf(s_job.message, sizeof(s_job.message), "%s", msg);
}

// ---- WiFi 事件 ----

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_events, EV_DISCONNECT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, EV_GOT_IP);
    }
}

// ---- NVS 凭证(自有命名空间;WIFI_STORAGE_RAM,设备只在此显式读写) ----

static const char k_nvs_ns[] = "metapass";

static bool creds_load(char ssid[33], char pass[65])
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l1 = 33, l2 = 65;
    const bool ok = nvs_get_str(h, "sta_ssid", ssid, &l1) == ESP_OK
                 && nvs_get_str(h, "sta_pass", pass, &l2) == ESP_OK
                 && ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "sta_ssid", ssid);
    nvs_set_str(h, "sta_pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

// ---- 一次性准备(NVS/netif/event loop,失败绝不自动擦除) ----

static esp_err_t net_prepare(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除分区", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    return esp_event_loop_create_default();
}

// ---- SoftAP 配网(httpd 仅两个路由:表单 + 凭证提交) ----

static const char PROV_HTML[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>meta-pass wifi setup</title>"
    "<body style='font-family:sans-serif;max-width:24em;margin:2em auto'>"
    "<h3>meta-pass WiFi setup</h3>"
    "<form onsubmit='event.preventDefault();save()'>"
    "WiFi name (SSID):<br><input id=s required maxlength=32><br><br>"
    "Password:<br><input id=p type=password maxlength=64><br><br>"
    "<button>Connect</button> <b id=st></b></form>"
    "<script>"
    "function save(){st.textContent='saving...';"
    "fetch('/api/wifi?ssid='+encodeURIComponent(s.value)"
    "+'&pass='+encodeURIComponent(p.value),{method:'POST'})"
    ".then(r=>{st.textContent=r.ok?'saved, device connecting...':'failed ('+r.status+')';});}"
    "</script>";

static int url_hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 有界字符串拷贝,永远 NUL 结尾(超出 dst_size-1 即截断)。用于把 URL 解码结果
// 收进定长缓冲——正常表单不会超长,截断是纵深防御。
static void copy_capped(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// 定长字段拷贝(esp wifi_config_t ssid/password 语义):最多拷满 dst_size 字节、
// 不强制 NUL(IDF 允许 32/64 字节满长无终止符;短输入依赖 wifi_config_t 零初始化
// 保证剩余字节为 0)。显式 memcpy 而非 snprintf:满长输入对 snprintf 是截断,
// 对本字段却是合法完整拷贝。
static void copy_field(void *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n > dst_size) n = dst_size;
    memcpy(dst, src, n);
}

static void url_decode_inplace(char *s)
{
    size_t w = 0;
    for (size_t r = 0; s[r] != '\0';) {
        if (s[r] == '%') {
            const int hi = url_hex_nib(s[r + 1]);
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

static esp_err_t h_prov_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PROV_HTML, HTTPD_RESP_USE_STRLEN);
}

// POST /api/wifi?ssid=..&pass=.. :表单提交 → 复制凭证 → 置位,由网络任务切换 STA。
// 不在 handler 里动 WiFi(httpd 任务栈与全局状态纪律:handler 只做参数搬运)。
static esp_err_t h_prov_wifi(httpd_req_t *req)
{
    // 容量核算:ssid ≤32 → encode ≤96;pass ≤64 → encode ≤192;加 key 与 '&' 余量。
    char arg[340] = {0};
    char ssid[33] = {0};
    char pass[65] = {0};
    if (httpd_req_get_url_query_str(req, arg, sizeof(arg)) != ESP_OK) {
        goto bad;
    }
    char raw[208] = {0};
    if (httpd_query_key_value(arg, "ssid", raw, sizeof(raw)) != ESP_OK) goto bad;
    url_decode_inplace(raw);
    copy_capped(ssid, sizeof(ssid), raw);
    raw[0] = '\0';
    if (httpd_query_key_value(arg, "pass", raw, sizeof(raw)) != ESP_OK) goto bad;
    url_decode_inplace(raw);
    copy_capped(pass, sizeof(pass), raw);
    if (ssid[0] == '\0') goto bad;

    snprintf(s_prov_ssid, sizeof(s_prov_ssid), "%s", ssid);
    snprintf(s_prov_pass, sizeof(s_prov_pass), "%s", pass);
    xEventGroupSetBits(s_events, EV_CREDENTIALS);
    return httpd_resp_sendstr(req, "ok");

bad:
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "bad request");
}

static void gen_ap_credentials(void)
{
    static const char k_set[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 8; i++) {
        s_status.password[i] = k_set[esp_random() % (sizeof(k_set) - 1)];
    }
    s_status.password[8] = '\0';
    snprintf(s_status.ssid, sizeof(s_status.ssid), "metapass-%04lX",
             (unsigned long)(esp_random() & 0xFFFF));
}

static void wifi_teardown(void)
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
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
}

static esp_err_t ap_start(void)
{
    wifi_teardown();
    gen_ap_credentials();

    s_netif = esp_netif_create_default_wifi_ap();
    if (!s_netif) return ESP_ERR_NO_MEM;

    const wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) goto fail;
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = AP_MAX_CONN,
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
    hcfg.max_uri_handlers = 4;
    hcfg.max_open_sockets = 3;
    hcfg.backlog_conn = 2;
    hcfg.lru_purge_enable = true;
    hcfg.stack_size = 4096;
    hcfg.recv_wait_timeout = 10;
    hcfg.send_wait_timeout = 10;
    err = httpd_start(&s_httpd, &hcfg);
    if (err != ESP_OK) goto fail;
    const httpd_uri_t uri_index = { "/", HTTP_GET,  h_prov_index, NULL };
    const httpd_uri_t uri_wifi  = { "/api/wifi", HTTP_POST, h_prov_wifi, NULL };
    httpd_register_uri_handler(s_httpd, &uri_index);
    httpd_register_uri_handler(s_httpd, &uri_wifi);

    set_state(SN_STATE_AP_UP, "WiFi setup AP ready.");
    ESP_LOGI(TAG, "配网 AP 就绪: %s(%s)", s_status.ssid, s_status.password);
    return ESP_OK;

fail:
    wifi_teardown();
    return err;
}

// STA 连接 + SNTP 同步;成功返回 ESP_OK 并置 ONLINE。
static esp_err_t sta_online(const char *ssid, const char *pass)
{
    wifi_teardown();
    ESP_LOGI(TAG, "连接路由器: %s", ssid);

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;

    const wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;
    wifi_config_t sta = {0};
    copy_field(sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
    copy_field(sta.sta.password, sizeof(sta.sta.password), pass);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) goto fail;

    snprintf(s_status.sta_ssid, sizeof(s_status.sta_ssid), "%s", ssid);
    set_state(SN_STATE_CONNECTING, "Connecting to WiFi...");
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_up = true;

    // 清掉上一轮连接残留的 GOT_IP/DISCONNECT 位,防本次等待被旧事件瞬时击穿。
    xEventGroupClearBits(s_events, EV_GOT_IP | EV_DISCONNECT);
    // 等 IP:30s 死线内循环等待;每次断连事件都重发 esp_wifi_connect(不限次数)。
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STA_CONNECT_MS);
    bool got_ip = false;
    while (xTaskGetTickCount() < deadline) {
        const EventBits_t bits = xEventGroupWaitBits(s_events,
                                                     EV_GOT_IP | EV_DISCONNECT,
                                                     pdTRUE, pdFALSE,
                                                     pdMS_TO_TICKS(1000));
        if (bits & EV_GOT_IP) {
            got_ip = true;
            break;
        }
        if (bits & EV_DISCONNECT) {
            ESP_LOGW(TAG, "STA 断连,重试...");
            esp_wifi_connect();
        }
    }
    if (!got_ip) {
        err = ESP_ERR_TIMEOUT;
        goto fail;
    }

    // SNTP:mbedTLS 默认开启证书时间校验(MBEDTLS_HAVE_TIME_DATE),不同步会先
    // 撞 BADCERT_FUTURE——TLS 之前必须拿到真实时间。
    set_state(SN_STATE_CONNECTING, "Syncing clock...");
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) goto fail;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_SYNC_MS));
    esp_netif_sntp_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 同步超时(%s),仍以已同步度最高的本地时钟继续",
                 esp_err_to_name(err));
        // 不视为致命:多数网络环境 SNTP 可达;失败时 TLS 时间校验可能拒绝证书,
        // 错误会经 analyze/install 的 ESP_FAIL 上屏,用户可重试。
    }

    creds_save(ssid, pass);
    set_state(SN_STATE_ONLINE, "Online.");
    ESP_LOGI(TAG, "ONLINE(路由器 %s)", ssid);
    return ESP_OK;

fail:
    wifi_teardown();
    return err;
}

// ---- 网络任务:凭证处理 + 作业队列 ----

typedef struct {
    uint8_t  cmd;          // 1=analyze 2=install
    uint32_t play_id;
    int      slot;
} job_msg_t;

static void job_run_analyze(uint32_t play_id)
{
    set_job(SN_JOB_RUNNING, "Fetching info...");
    meta_store_analysis_t out;
    if (meta_store_api_analyze(play_id, &out) == ESP_OK) {
        s_analysis = out;
        s_analysis_valid = true;
        set_job(SN_JOB_DONE_OK, "OK");
    } else {
        s_analysis_valid = false;
        // analyze 失败时 reason 字段已是可展示的原因码(not-found/format/...)。
        set_job(SN_JOB_DONE_FAIL, out.reason);
    }
}

static void job_run_install(uint32_t play_id, int slot)
{
    if (!s_analysis_valid) {
        set_job(SN_JOB_DONE_FAIL, "analyze first");
        return;
    }
    set_job(SN_JOB_RUNNING, "Installing...");
    esp_err_t err = meta_store_api_install(play_id, slot, &s_analysis, s_slots);
    if (err == ESP_OK) {
        set_job(SN_JOB_DONE_OK, "Installed.");
    } else {
        // 上屏用 install 落进进度快照的精准文案(Cancelled. / Checksum mismatch. / ...),
        // esp_err_to_name 只作兜底。
        meta_store_api_progress_t p;
        meta_store_api_poll(&p);
        set_job(SN_JOB_DONE_FAIL, p.message[0] ? p.message : esp_err_to_name(err));
    }
}

static void job_task_main(void *arg)
{
    (void)arg;
    job_msg_t msg;
    for (;;) {
        // 等作业或配网凭证(两者共用事件组等待,凭证优先处理)。
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, EV_CREDENTIALS | EV_STOP, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(500));

        if (bits & EV_STOP) continue;   // stop() 会同步完成资源释放,此处仅唤醒

        if (bits & EV_CREDENTIALS) {
            char ssid[33], pass[65];
            snprintf(ssid, sizeof(ssid), "%s", s_prov_ssid);
            snprintf(pass, sizeof(pass), "%s", s_prov_pass);
            if (sta_online(ssid, pass) == ESP_OK) {
                xQueueReset(s_job_queue);   // 换网络后旧作业作废
            } else {
                set_state(SN_STATE_ERROR, "WiFi connect failed.");
                ESP_LOGW(TAG, "STA 连接失败,回落配网页");
                ap_start();   // 失败回落:重新开配网 AP
            }
            continue;
        }

        if (s_status.state != SN_STATE_ONLINE) continue;

        while (xQueueReceive(s_job_queue, &msg, 0) == pdTRUE) {
            if (msg.cmd == 1) job_run_analyze(msg.play_id);
            else if (msg.cmd == 2) job_run_install(msg.play_id, msg.slot);
        }
    }
}

// ---- 对外 API ----

esp_err_t meta_store_net_init(meta_slot_info_t slots[META_SLOT_COUNT])
{
    if (!slots) return ESP_ERR_INVALID_ARG;
    if (s_initialized) return ESP_OK;
    s_slots = slots;

    esp_err_t err = net_prepare();
    if (err != ESP_OK) return err;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    s_job_queue = xQueueCreate(JOB_QUEUE_LEN, sizeof(job_msg_t));
    if (!s_job_queue) return ESP_ERR_NO_MEM;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL);
    if (err != ESP_OK) return err;

    const BaseType_t t = xTaskCreate(job_task_main, "store_net", JOB_STACK, NULL,
                                     JOB_PRIO, &s_job_task);
    if (t != pdPASS) return ESP_ERR_NO_MEM;

    s_initialized = true;
    set_state(SN_STATE_IDLE, "");
    set_job(SN_JOB_IDLE, "");
    return ESP_OK;
}

esp_err_t meta_store_net_begin(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_status.state == SN_STATE_ONLINE
        || s_status.state == SN_STATE_CONNECTING
        || s_status.state == SN_STATE_AP_UP) {
        return ESP_OK;   // 幂等:流程已在跑
    }

    char ssid[33], pass[65];
    if (creds_load(ssid, pass)) {
        // 异步连:作业任务只处理 EV_CREDENTIALS,这里直接投递一个"内部连接"消息,
        // 复用同一处理路径(经 s_prov_* 传递凭证)。
        snprintf(s_prov_ssid, sizeof(s_prov_ssid), "%s", ssid);
        snprintf(s_prov_pass, sizeof(s_prov_pass), "%s", pass);
        xEventGroupSetBits(s_events, EV_CREDENTIALS);
        set_state(SN_STATE_CONNECTING, "Connecting to WiFi...");
        return ESP_OK;
    }
    esp_err_t err = ap_start();
    if (err != ESP_OK) {
        set_state(SN_STATE_ERROR, "Setup AP failed.");
        return err;
    }
    return ESP_OK;
}

void meta_store_net_stop(void)
{
    if (!s_initialized) return;
    xEventGroupSetBits(s_events, EV_STOP);
    xQueueReset(s_job_queue);
    wifi_teardown();
    set_state(SN_STATE_IDLE, "");
    set_job(SN_JOB_IDLE, "");
    s_analysis_valid = false;
}

esp_err_t meta_store_net_cmd_analyze(uint32_t play_id)
{
    if (s_status.state != SN_STATE_ONLINE) return ESP_ERR_INVALID_STATE;
    if (s_job.state == SN_JOB_RUNNING) return ESP_ERR_INVALID_STATE;
    const job_msg_t msg = { .cmd = 1, .play_id = play_id, .slot = -1 };
    if (xQueueSend(s_job_queue, &msg, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    // 入队即置 RUNNING(网络任务真正执行前,UI 不应再看到上一个作业的残留终态)。
    set_job(SN_JOB_RUNNING, "Queued.");
    return ESP_OK;
}

esp_err_t meta_store_net_cmd_install(uint32_t play_id, int slot)
{
    if (s_status.state != SN_STATE_ONLINE) return ESP_ERR_INVALID_STATE;
    if (s_job.state == SN_JOB_RUNNING || !s_analysis_valid) return ESP_ERR_INVALID_STATE;
    if (slot < 0 || slot >= META_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    const job_msg_t msg = { .cmd = 2, .play_id = play_id, .slot = slot };
    if (xQueueSend(s_job_queue, &msg, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    set_job(SN_JOB_RUNNING, "Queued.");
    return ESP_OK;
}

const meta_store_analysis_t *meta_store_net_analysis(void)
{
    return s_analysis_valid ? &s_analysis : NULL;
}

void meta_store_net_poll(meta_store_net_status_t *out)
{
    if (out) *out = s_status;
}

void meta_store_net_job_poll(meta_store_net_job_t *out)
{
    if (out) *out = s_job;
}
