// main/meta_store_net.c —— 实现见头文件注释。
// 事件流:begin() → [有凭证] STA 连接等 IP_EVENT_STA_GOT_IP → SNTP 同步 → ONLINE;
//         begin() → [无凭证/连接失败] SoftAP(APSTA,后台周期扫 AP)+ httpd
//         (页面/扫描列表/凭证提交 + 302 兜底)+ DNS 劫持(Captive Portal 弹窗)→
//         POST /api/wifi → 停 AP/启 STA(同上)。作业经队列进网络任务执行。
#include "meta_store_net.h"

#include "meta_store_prov.h"

#include <stdio.h>
#include <stdlib.h>
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
#include "lwip/sockets.h"   // Captive Portal 的 DNS 劫持用 BSD socket API
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "store_net";

#define AP_MAX_CONN        1
#define STA_CONNECT_MS     30000
#define SNTP_SYNC_MS       5000    // 死等上限:可达时<1s,不可达时不陪葬(旧 15s)
#define JOB_QUEUE_LEN      2
// r10.8:8192→10240 —— 样例恢复器(真机验证)用 10240 跑同款 TLS 栈,8K 在
// 握手峰值 + 响应解析叠加时压线。F2 对齐实测值。
#define JOB_STACK          10240  // analyze/install(TLS+mbedTLS 握手峰值 + 响应解析)在此任务内跑
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
static volatile int  s_last_disconnect_reason;   // 最近一次 STA 断连原因码(诊断上屏)
static volatile bool s_teardown_req;   // stop()/reset_wifi() 请求:任务内 teardown+回落 AP

static void set_state(sn_state_t st, const char *msg)
{
    s_status.state = st;
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg);
}

static void set_job(sn_job_state_t st, const char *msg)
{
    s_job.state = st;
    snprintf(s_job.message, sizeof(s_job.message), "%s", msg);
    s_job.detail[0] = '\0';   // 默认清空;失败带层位时由调用方紧跟 set_job_detail
}

// r10.4:失败层位入快照(msg = reason 码,detail = 服务端诊断句/分类文案)。
static void set_job_detail(sn_job_state_t st, const char *msg, const char *detail)
{
    s_job.state = st;
    snprintf(s_job.message, sizeof(s_job.message), "%s", msg);
    snprintf(s_job.detail, sizeof(s_job.detail), "%s", detail ? detail : "");
}

// ---- WiFi 事件 ----

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // 记录断连原因码(密码错=15 等),sta_online 失败上屏用。
        if (data) {
            const wifi_event_sta_disconnected_t *d = data;
            s_last_disconnect_reason = d ? d->reason : 0;
        }
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

// 配网页(方案 v3.2-r7):手机连上热点后多数被 Captive Portal 自动弹到本页,
// 不弹的系统手动访问 http://192.168.4.1。页面能力:
//   「Scan networks」→ GET /api/scan 渲染点选列表(选完只输密码);
//   手输 SSID 仍保留(扫描未就绪/隐藏网络兜底)。
// 提交成功后设备关热点切 STA,页面提示手机重连路由器等待设备上线。
// 所有动态文本经 textContent 注入(非 innerHTML),SSID 不可注入页面。
static const char PROV_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>meta-pass WiFi setup</title></head>"
    "<body style='font-family:sans-serif;max-width:24em;margin:2em auto'>"
    "<h3>meta-pass WiFi setup</h3>"
    "<button onclick='scan()'>Scan networks</button> <span id=hint></span><br><br>"
    "<div id=list></div>"
    "<form onsubmit='event.preventDefault();save()'>"
    "WiFi name (SSID):<br><input id=s required maxlength=32><br><br>"
    "Password:<br><input id=p type=password maxlength=64><br><br>"
    "<button>Connect</button> <b id=st></b></form>"
    "<script>"
    "function scan(){hint.textContent='scanning...';list.innerHTML='';"
    "fetch('/api/scan').then(function(r){return r.json();}).then(function(j){"
    "if(!j.networks||!j.networks.length){"
    "hint.textContent='no networks yet - tap scan again, or type SSID below.';return;}"
    "hint.textContent='pick your network:';"
    "j.networks.forEach(function(n){var b=document.createElement('button');"
    "b.textContent=n.ssid+'  ('+n.rssi+'dBm, '+(n.auth?'secured':'open')+')';"
    "b.style.display='block';b.style.margin='4px 0';"
    "b.onclick=function(){s.value=n.ssid;p.focus();};"
    "list.appendChild(b);});"
    "}).catch(function(){hint.textContent='scan failed; type SSID manually.';});}"
    "function save(){st.textContent='saving...';"
    "fetch('/api/wifi?ssid='+encodeURIComponent(s.value)"
    "+'&pass='+encodeURIComponent(p.value),{method:'POST'})"
    ".then(function(r){return r.text().then(function(t){return [r.ok,t];});})"
    ".then(function(ok){st.textContent=ok[0]"
    "?'Received. Device is connecting to '+s.value"
    "+' — WATCH THE DEVICE SCREEN: it shows progress, and the reason if it fails. Reconnect your phone to '+s.value+' after the device goes online.'"
    ":'failed ('+ok[1]+')';});}"
    "</script></body></html>";

// ---- Captive Portal:DNS 劫持(UDP 53 把所有 A 查询以无应答应答包回给 AP 侧设备)----
// 手机连上热点后,系统后台探测判定"存在 captive portal"即自动弹出配置页;
// 不弹的系统仍可手动访问 http://192.168.4.1(302 兜底见 h_prov_catchall)。
#define DNS_PORT        53
#define DNS_TASK_STACK  3072
#define DNS_TASK_PRIO   3
static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static int s_dns_sock = -1;

static void dns_task_main(void *arg)
{
    (void)arg;
    uint8_t pkt[512];
    struct sockaddr_storage from;
    socklen_t fromlen;
    while (s_dns_run) {
        fromlen = sizeof(from);
        const ssize_t n = recvfrom(s_dns_sock, pkt, sizeof(pkt), 0,
                                   (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        const size_t end = meta_store_dns_query_end(pkt, (size_t)n);
        if (end == 0) continue;
        pkt[2] |= 0x80;      // QR=1(响应);RA=1
        pkt[3] |= 0x80;
        pkt[6] = pkt[7] = 0; // ANCOUNT/NSCOUNT/ARCOUNT = 0:无应答记录,
        pkt[8] = pkt[9] = 0; // 手机只能把它当"captive portal"判定信号
        pkt[10] = pkt[11] = 0;
        (void)sendto(s_dns_sock, pkt, end, 0, (struct sockaddr *)&from, fromlen);
    }
    vTaskDelete(NULL);       // 任务自清理
}

static esp_err_t dns_relay_start(void)
{
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) return ESP_FAIL;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_FAIL;
    }
    s_dns_run = true;
    if (xTaskCreate(dns_task_main, "store_dns", DNS_TASK_STACK, NULL,
                    DNS_TASK_PRIO, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Captive Portal DNS ready (UDP 53)");
    return ESP_OK;
}

static void dns_relay_stop(void)
{
    s_dns_run = false;
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, 0);
        close(s_dns_sock);   // 唤醒阻塞 recvfrom,任务见 !s_dns_run 自行退出
        s_dns_sock = -1;
    }
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

// 手机系统的 captive portal 探测请求带运营商/厂商 Host(如 captive.apple.com);
// 设备自配网页入口固定是 192.168.4.1。见非本机 Host → 302 弹配置页。
static esp_err_t h_prov_catchall(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Content-Length", "0");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t h_prov_index(httpd_req_t *req)
{
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK
        && strncmp(host, "192.168.4.1", sizeof("192.168.4.1")) != 0) {
        return h_prov_catchall(req);   // 探测请求:302 弹窗
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PROV_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /api/scan → 同步扫一轮的 APSTA 结果 JSON:
// {"networks":[{"ssid":"...","rssi":-52,"auth":true},...]}
// handler 内同步完成 起扫→等→取(收尾语义见 h_prov_scan 注释;残留扫描态
// 会卡死下一次扫描与 STA 连接,故 get_ap_records 必须无条件执行)。
#define PROV_SCAN_MAX       20

// 有界 JSON 组装器:溢出即停止写入(页面把残缺列表按空处理,无危害)。
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} json_buf_t;

static void jb_add(json_buf_t *jb, const char *s)
{
    if (jb->len + 1 >= jb->cap) return;
    size_t n = strlen(s);
    if (n > jb->cap - jb->len - 1) n = jb->cap - jb->len - 1;
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
}

static void jb_add_int(json_buf_t *jb, int v)
{
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "%d", v);
    jb_add(jb, tmp);
}

static esp_err_t h_prov_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // 同步扫描:在本 handler 内完成 起扫→等→取 的全流程。IDF 里"扫描完成但
    // 记录未取"的残留态会同时卡死下一次扫描与 STA 连接 —— r8 真机踩雷:
    // 旧的跨任务按需方案(job 任务扫、handler 取)在空结果分支不取记录,热点
    // 列表从此永远空白,提交凭证后 connect 又被残留扫描态拖住,表现为
    // "目标网络长时间找不到 / 长时间连不上"。收尾必须与起扫在同一处。
    esp_wifi_scan_start(NULL, false);
    // 阻塞等扫描完成(全信道 ~1.5-2s)。httpd 在独立任务,页面 fetch 无超时,
    // 配网门户单用途,期间阻塞可接受;手机已连热点,beacon 暂停不断开已建链。
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint16_t count = PROV_SCAN_MAX;
    wifi_ap_record_t *records = malloc((size_t)count * sizeof(wifi_ap_record_t));
    if (!records) {
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }
    // 无论扫到多少条(含 0),get_ap_records 成功返回即完成清态 —— 不给后续
    // 扫描/连接留残态(这是本次修复的核心语义,勿改回 count==0 提前返回)。
    const esp_err_t fetch = esp_wifi_scan_get_ap_records(&count, records);
    if (fetch != ESP_OK) {
        ESP_LOGW(TAG, "scan fetch failed: %s", esp_err_to_name(fetch));
        free(records);
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }
    if (count == 0) {
        free(records);
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }

    // 每条上界:转义 ssid(96)+固定 JSON(~48);外加首尾包装。
    const size_t cap = 32 + (size_t)count * (96 + 48);
    char *json = malloc(cap);
    if (!json) {
        free(records);
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }

    json_buf_t jb = { .buf = json, .cap = cap, .len = 0 };
    json[0] = '\0';
    jb_add(&jb, "{\"networks\":[");
    for (uint16_t i = 0; i < count; i++) {
        if (i) jb_add(&jb, ",");
        char esc[97];
        meta_store_prov_json_escaped_ssid(records[i].ssid, esc, sizeof(esc));
        jb_add(&jb, "{\"ssid\":\"");
        jb_add(&jb, esc);
        jb_add(&jb, "\",\"rssi\":");
        jb_add_int(&jb, records[i].rssi);
        jb_add(&jb, ",\"auth\":");
        jb_add(&jb, records[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
        jb_add(&jb, "}");
    }
    jb_add(&jb, "]}");
    free(records);
    const esp_err_t ret = httpd_resp_send(req, json, jb.len);
    free(json);
    return ret;
}

// POST /api/wifi?ssid=..&pass=.. :表单提交 → 复制凭证 → 置位,由网络任务切换 STA。
// 不在 handler 里动 WiFi(httpd 任务栈与全局状态纪律:handler 只做参数搬运)。
static esp_err_t h_prov_wifi(httpd_req_t *req)
{
    // 表单解析/校验在 meta_store_prov(共享纯逻辑,host 单测覆盖):
    // 键缺失/空值/重复键/解码后超容量一律拒绝(超限拒绝而非截断 —— 截断会把
    // 长 SSID 悄悄改写成连不上的另一个名字)。
    char arg[340] = {0};
    char ssid[33] = {0};
    char pass[65] = {0};
    if (httpd_req_get_url_query_str(req, arg, sizeof(arg)) != ESP_OK) {
        goto bad;
    }
    if (!meta_store_prov_parse_wifi_query(arg, ssid, sizeof(ssid), pass, sizeof(pass))) {
        goto bad;
    }

    if (pass[0] == '\0') goto bad;   // 开放网络极不安全且极少见,要求密码(页面同口径)

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
    // r7:开放热点(无密码),降低配网门槛;SSID 仍随机化避免多台设备互扰。
    snprintf(s_status.ssid, sizeof(s_status.ssid), "metapass-%04lX",
             (unsigned long)(esp_random() & 0xFFFF));
}

static void wifi_teardown(void)
{
    dns_relay_stop();
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
    // APSTA:纯 AP 模式无法扫描;station 接口仅服务于后台扫 AP,从不设置 sta
    // 配置也从不 connect,不影响 SoftAP 本身。
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) goto fail;
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            // 开放网络:Captive Portal 靠"不加密 + 劫持"自动弹窗;WPA2 密码会
            // 阻断多数系统的 portal 探测与自动弹窗。旧版随机密码机制移除,
            // 防蹭网面 = AP_MAX_CONN=1 + 配网时长有限(凭证到手即关 AP)。
            .max_connection = AP_MAX_CONN,
            .beacon_interval = 100,
        },
    };
    copy_field(ap.ap.ssid, sizeof(ap.ap.ssid), s_status.ssid);
    ap.ap.ssid_len = strlen(s_status.ssid);
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
    const httpd_uri_t uri_scan  = { "/api/scan", HTTP_GET,  h_prov_scan,  NULL };
    const httpd_uri_t uri_wifi  = { "/api/wifi", HTTP_POST, h_prov_wifi, NULL };
    const httpd_uri_t uri_any   = { "*", HTTP_GET,  h_prov_catchall, NULL };
    httpd_register_uri_handler(s_httpd, &uri_index);
    httpd_register_uri_handler(s_httpd, &uri_scan);
    httpd_register_uri_handler(s_httpd, &uri_wifi);
    httpd_register_uri_handler(s_httpd, &uri_any);

    // DNS 劫持失败只损失"自动弹窗",页面仍可手动访问,不视为致命。
    if (dns_relay_start() != ESP_OK) {
        ESP_LOGW(TAG, "DNS 劫持启动失败:仅能手动访问 192.168.4.1");
    }

    set_state(SN_STATE_AP_UP, "WiFi setup AP ready.");
    ESP_LOGI(TAG, "配网 AP 就绪(开放热点): %s", s_status.ssid);
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
    // PMF capable:家用路由器普遍默认"WPA2/WPA3 混合 + 强制/可选 PMF"。
    // wifi_config_t 零初始化时 pmf_cfg.capable=false,对开启 PMF 的 BSS 一律
    // 4-way 握手超时(reason 201/202),外在表现正是"长时间连不上"。capable=
    // true 表示"对方要求才用",required=false 仍兼容纯 WPA2 老路由。
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) goto fail;

    snprintf(s_status.sta_ssid, sizeof(s_status.sta_ssid), "%s", ssid);
    set_state(SN_STATE_CONNECTING, "Connecting to WiFi...");
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_up = true;

    // 清掉上一轮连接残留的 GOT_IP/DISCONNECT 位,防本次等待被旧事件瞬时击穿。
    xEventGroupClearBits(s_events, EV_GOT_IP | EV_DISCONNECT);
    // 等 IP:30s 死线内循环等待;断连事件驱动的重试(无定时重发)。
    // 等待前被清位吞掉的断连事件由首次 waitBits 的粘性位兜住。
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
            ESP_LOGW(TAG, "STA 断连(reason=%d),重试...", s_last_disconnect_reason);
            esp_wifi_connect();
        }
        // 无事件的一秒:什么都不做。事件组位是粘性的(等待前丢失的
        // DISCONNECT 会在首次 waitBits 立即返回),无需定时兜底重发 ——
        // IDF 里 station 连接中再调 esp_wifi_connect 会"断开重连",
        // 定时重发把内部信道扫描(1-3s)反复打断,关联永远不收敛
        // (静态审查 F1;"尝试连接长时间连不上"的直接嫌疑)。
    }
    if (!got_ip) {
        err = ESP_ERR_TIMEOUT;
        goto fail;
    }

    // SNTP:证书时间校验取决于 CONFIG_MBEDTLS_HAVE_TIME_DATE(Kconfig 默认 n,
    // 本仓未开)—— 即便如此时钟仍要同步:日志时间戳、TLV 相对时间、后续策略
    // 都依赖它。若未来开启 TIME_DATE,BADCERT_FUTURE 将由这里前置拦截。
    // 服务器:ntp.aliyun.com 为主(国内 <1s;pool.ntp.org 全球轮询,国内常 2-10s
    // 甚至丢包),pool.ntp.org 备份;LWIP_SNTP_MAX_SERVERS 需 >=2(sdkconfig)。
    set_state(SN_STATE_CONNECTING, "Syncing clock...");
    // 双服务器:主 ntp.aliyun.com(国内 <1s),备 pool.ntp.org。不走 MULTIPLE
    // 宏 —— 花括号内的逗号会被预处理器当宏参数分隔,是坑;直接字段赋值。
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    sntp_cfg.servers[1] = "pool.ntp.org";   // CONFIG_LWIP_SNTP_MAX_SERVERS=2
    sntp_cfg.num_of_servers = 2;
    err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) goto fail;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_SYNC_MS));
    esp_netif_sntp_deinit();
    if (err != ESP_OK) {
        // 超时不致命,但要诚实上屏:时钟停在 1970 时下一步 analyze 的 TLS 会以
        // BADCERT_FUTURE 失败,用户需要知道因果,而不是看到莫名 unavailable。
        ESP_LOGW(TAG, "SNTP 同步超时(%s),时钟可能未同步", esp_err_to_name(err));
        set_state(SN_STATE_CONNECTING, "Clock unsynced (TLS may fail).");
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
        // r10.4:失败原因 + 层位 detail 都入快照 —— 服务端诊断句
        // ("upstream 503 (metadata)")或设备分类文案("TLS failed (clock
        // unsynced).")不再被丢弃,UI 能一次定位失败层。
        set_job_detail(SN_JOB_DONE_FAIL, out.reason, out.detail);
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

        if (bits & EV_STOP) {
            // F3:teardown/ap_start 只在本任务做(与 sta_online 串行,无竞态)。
            if (s_teardown_req) {
                s_teardown_req = false;
                wifi_teardown();
                xEventGroupClearBits(s_events, EV_STOP | EV_CREDENTIALS
                                              | EV_GOT_IP | EV_DISCONNECT);
                ap_start();
            }
            continue;
        }

        if (bits & EV_CREDENTIALS) {
            char ssid[33], pass[65];
            snprintf(ssid, sizeof(ssid), "%s", s_prov_ssid);
            snprintf(pass, sizeof(pass), "%s", s_prov_pass);
            if (sta_online(ssid, pass) == ESP_OK) {
                xQueueReset(s_job_queue);   // 换网络后旧作业作废
            } else {
                char fail_buf[32];   // 未知原因码的格式化缓冲
                set_state(SN_STATE_ERROR, meta_store_wifi_fail_text(s_last_disconnect_reason, fail_buf, sizeof(fail_buf)));
                ESP_LOGW(TAG, "STA 连接失败(reason=%d),回落配网页",
                         s_last_disconnect_reason);
                if (!s_teardown_req) {
                    ap_start();   // 失败回落:重新开配网 AP(stop/reset 请求优先)
                }
            }
            continue;
        }

        // 配网页:扫描已收进 h_prov_scan(handler 内同步 起扫→等→取)。
        // r8 教训:跨任务接力 + 空结果分支不取记录,残留扫描态既冻结节后
        // 所有扫描,也卡死 STA 连接;收尾与起扫必须在同一处。
        vTaskDelay(pdMS_TO_TICKS(200));

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
    // 生命周期归一(F3):teardown 只发生在 job 任务内,与 sta_online/ap_start
    // 天然互斥。置 STOP + s_teardown_req,任务在下一个 500ms 节拍收尾。
    s_teardown_req = true;
    xEventGroupSetBits(s_events, EV_STOP);
    xQueueReset(s_job_queue);
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

esp_err_t meta_store_net_reset_wifi(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_status.state == SN_STATE_AP_UP) return ESP_OK;   // 已在配网态,幂等

    // 拆旧连接(可能正在 STA/CONNECTING/ONLINE)。生命周期归一(F3):不在此处
    // 直接 teardown —— job 任务可能正在 sta_online 的 45s 等待里,跨任务
    // deinit 会与失败回落 ap_start 撞车。置 s_teardown_req,任务在每个节拍
    // 检查:teardown → 清位 → ap_start,全程单任务串行,天然互斥。
    s_teardown_req = true;
    xEventGroupSetBits(s_events, EV_STOP);

    // 擦凭证:nvs_open RW 失败不致命(可能分区刚初始化),下次配网保存时自然建。
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "sta_ssid");
        nvs_erase_key(h, "sta_pass");
        nvs_commit(h);
        nvs_close(h);
    }

    // AP 重开由 job 任务在 teardown 后统一做(本函数不再直接 ap_start)。
    // 这里自旋等任务完成归一动作(上限 ~3s,teardown 最长几秒内完成;
    // 典型路径 sta_online 未在跑,任务一个节拍内即完成)。
    for (int i = 0; i < 30 && s_teardown_req; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_status.state != SN_STATE_AP_UP) {
        // 任务没能完成(理论上不可达):状态行给出事实,用户重试。
        set_state(SN_STATE_ERROR, "Reset stuck, retry.");
        return ESP_FAIL;
    }
    return ESP_OK;
}
