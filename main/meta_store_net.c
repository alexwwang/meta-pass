// main/meta_store_net.c —— 实现见头文件注释。
// 事件流:begin() → [有凭证] STA 连接等 IP_EVENT_STA_GOT_IP → ONLINE;
//         begin() → [无凭证/连接失败] SoftAP(APSTA,后台周期扫 AP)+ httpd
//         (页面/扫描列表/凭证提交 + 302 兜底)+ DNS 劫持(Captive Portal 弹窗)→
//         POST /api/wifi → 停 AP/启 STA(同上)。
// feat/mota 净切:WAN analyze/install 作业链与 SNTP 同步已移除 —— 设备不再经
// TLS 出网;ONLINE 后由 UI 启动本地 LAN install 服务(meta_store_install)。
#include "meta_store_net.h"

#include "meta_store_prov.h"
#include "meta_carve_store.h"   // META_CRED_BAK_OFFSET(凭证备份新家,设计 §4.3 L6)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_flash.h"      // 裸 flash 凭证备份(读/写/擦 sector)
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_crc.h"    // 凭证备份 CRC32
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"   // Captive Portal 的 DNS 劫持用 BSD socket API
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "store_net";

#define AP_MAX_CONN        1
#define STA_CONNECT_MS     30000
// 网络任务只做 WiFi 启停/凭证接力/teardown(无 TLS 作业),栈与配网 httpd 同档。
#define NET_TASK_STACK     4096
#define NET_TASK_PRIO      5

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

static httpd_handle_t     s_httpd;
static esp_netif_t       *s_netif;          // 当前生效的 wifi netif(AP 或 STA)
static bool               s_wifi_up;
static bool               s_initialized;    // init() 完成(任务/event handler 在)
static TaskHandle_t       s_net_task;
static EventGroupHandle_t s_events;
static meta_store_net_status_t s_status;

// s_events 位
#define EV_GOT_IP      BIT0
#define EV_DISCONNECT  BIT1
#define EV_CREDENTIALS BIT2   // 配网页已收到凭证(内容在 s_prov_*)
#define EV_STOP        BIT3

static char s_prov_ssid[33];
static char s_prov_pass[65];
static volatile int  s_last_disconnect_reason;   // 最近一次 STA 断连原因码(诊断上屏)
// ONLINE 后断连自动重连用的最近成功凭证(sta_online 成功时快照)。
static char s_last_ssid[33];
static char s_last_pass[65];
static bool s_was_online;                        // sta_online 成功过(重连前置)
static volatile bool s_teardown_req;   // stop()/reset_wifi() 请求:任务内 teardown+回落 AP
static volatile bool s_ap_after_teardown; // 仅 reset_wifi(改网意图)要求 teardown 后重开 AP;
                                         // 普通离店(teardown)不得留热点 —— 否则重进商店时
                                         // begin() 见 AP_UP 幂等返回,用户被永远卡在配网页,
                                         // 而 WiFi 凭证明明还在(真机 bug,会话三观察全吻合)。

static void set_state(sn_state_t st, const char *msg)
{
    s_status.state = st;
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg);
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

// 裸 flash 凭证备份:store 分区扇区4(0x35E000,设计 §4.3 L6 新家)。
// 历史(dynslot):原在 cardid 与 ota_1 之间的裸空隙 0x35A000 —— 那块地
// 现在是 store 扇区0(carve 记录 A),记录一写就会覆盖。升级时 ensure 的
// cred_relocate 会先把旧备份整扇区搬到新家,再允许记录写入。
// 真机 bug 由来:子固件(社区玩法)启动会把共享 nvs 分区格式化,WiFi 凭证
// 随子固件槽位注册表一起被抹 → 重进商店要求重新配网。对策 = 双写:
// NVS 主读(快),裸区备份(nvs 读不到时自愈恢复)。单 sector 擦写,
// magic+len+crc 校验,ssid 空即视为无效。
#define CRED_BAK_OFFSET   META_CRED_BAK_OFFSET   // = 0x35E000(store 扇区4)
#define CRED_BAK_MAGIC    META_CRED_BAK_MAGIC   // "MPCK"(与 ensure 的 relocate 同源)
#define CRED_BAK_SSID_MAX 33
#define CRED_BAK_PASS_MAX 65

typedef struct {
    uint32_t magic;
    uint32_t ssid_len;
    uint32_t pass_len;
    uint32_t crc32;      // 覆盖 ssid_len/pass_len + ssid + pass
    char     ssid[CRED_BAK_SSID_MAX];
    char     pass[CRED_BAK_PASS_MAX];
} cred_backup_t;

static uint32_t cred_crc(const cred_backup_t *b)
{
    // esp_rom_crc32_le 初值 0 兼容标准 CRC-32/MPEG 校验用途,自洽即可。
    uint32_t c = esp_rom_crc32_le(0, (const uint8_t *)&b->ssid_len,
                                  sizeof(b->ssid_len) + sizeof(b->pass_len));
    c = esp_rom_crc32_le(c, (const uint8_t *)b->ssid, b->ssid_len);
    c = esp_rom_crc32_le(c, (const uint8_t *)b->pass, b->pass_len);
    return c;
}

static bool cred_backup_load(char ssid[33], char pass[65])
{
    cred_backup_t b;
    if (esp_flash_read(NULL, &b, CRED_BAK_OFFSET, sizeof(b)) != ESP_OK) return false;
    if (b.magic != CRED_BAK_MAGIC) return false;
    if (b.ssid_len == 0 || b.ssid_len >= CRED_BAK_SSID_MAX
        || b.pass_len >= CRED_BAK_PASS_MAX) return false;
    if (cred_crc(&b) != b.crc32) return false;
    if (b.ssid[b.ssid_len] != '\0' || (b.pass_len > 0 && b.pass[b.pass_len] != '\0')) return false;
    memcpy(ssid, b.ssid, b.ssid_len + 1);
    memcpy(pass, b.pass, b.pass_len + 1);
    return true;
}

static void cred_backup_save(const char *ssid, const char *pass)
{
    cred_backup_t b;
    memset(&b, 0xFF, sizeof(b));   // 与擦除态一致,未用字节不引入垃圾
    b.magic = CRED_BAK_MAGIC;
    b.ssid_len = (uint32_t)strlen(ssid);
    b.pass_len = (uint32_t)strlen(pass);
    if (b.ssid_len == 0 || b.ssid_len >= CRED_BAK_SSID_MAX
        || b.pass_len >= CRED_BAK_PASS_MAX) return;
    memcpy(b.ssid, ssid, b.ssid_len);
    memcpy(b.pass, pass, b.pass_len);
    b.ssid[b.ssid_len] = '\0';
    b.pass[b.pass_len] = '\0';
    b.crc32 = cred_crc(&b);
    if (esp_flash_erase_region(NULL, CRED_BAK_OFFSET, 4096) != ESP_OK) return;   // 1 sector
    esp_flash_write(NULL, &b, CRED_BAK_OFFSET, sizeof(b));
}

static void creds_save(const char *ssid, const char *pass);   // 定义在 creds_load 之后(备份自愈回写用)

static bool creds_load(char ssid[33], char pass[65])
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns, NVS_READONLY, &h) == ESP_OK) {
        size_t l1 = 33, l2 = 65;
        const bool ok = nvs_get_str(h, "sta_ssid", ssid, &l1) == ESP_OK
                     && nvs_get_str(h, "sta_pass", pass, &l2) == ESP_OK
                     && ssid[0] != '\0';
        nvs_close(h);
        if (ok) return true;
    }
    // NVS 缺失(子固件格式化 nvs 分区):裸区备份自愈 —— 恢复 NVS 主存,
    // 下次读路径不变。备份也读不到才算真无凭证。
    if (!cred_backup_load(ssid, pass)) return false;
    ESP_LOGW(TAG, "NVS 凭证丢失,已从裸 flash 备份恢复(%s)", ssid);
    creds_save(ssid, pass);
    return true;
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "sta_ssid", ssid);
        nvs_set_str(h, "sta_pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    cred_backup_save(ssid, pass);   // 双写:子固件抹 nvs 后可自愈
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

    // r10.12:APSTA 路径同样关省电 —— 该路径开完会话后 station 会重连,
    // PS 若留在默认 MIN,进入商店下载仍被 DTIM 节流(与 STA 直连路径同因)。
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) goto fail;

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

// STA 连接;拿到 IP 即置 ONLINE(feat/mota:已无 SNTP/TLS 依赖)。
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

    // r10.12:关省电。IDF 默认 WIFI_PS_MIN_MODEM,STA 在 DTIM 间隔休眠,
    // 下行 TCP 吞吐被睡眠周期节流(快→慢抖动的元凶);启动器单次会话模型下
    // 下载窗口短,吞吐优先于功耗。
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) goto fail;

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

    creds_save(ssid, pass);
    // 重连快照:ONLINE 后断连(bcn_timeout 等)自动重连用。
    snprintf(s_last_ssid, sizeof(s_last_ssid), "%s", ssid);
    snprintf(s_last_pass, sizeof(s_last_pass), "%s", pass);
    s_was_online = true;
    // 连接期重试会留下 EV_DISCONNECT 残留位;不清的话 job 循环一觉醒来看见
    // "在线+断连位"会立刻误触发一次自动重连。
    xEventGroupClearBits(s_events, EV_DISCONNECT);
    // r10.15c:回源诊断 —— resolve 后把 DNS 目标 IP 打上串口:198.18.x/198.19.x
    // = Fake-IP 段 = 流量进代理隧道(路由器劫持 DNS 场景一眼定罪)。
    // LAN 安装模式下此日志仍保留:配网链路异常时是第一手证据。
    {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dns_info_t dns = {0};
        if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            ESP_LOGI(TAG, "DNS: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
    }
    set_state(SN_STATE_ONLINE, "Online.");
    ESP_LOGI(TAG, "ONLINE(路由器 %s)", ssid);
    return ESP_OK;

fail:
    wifi_teardown();
    return err;
}

// ---- 网络任务:配网凭证接力与 teardown 接力(feat/mota:无 WAN 作业队列) ----

static void job_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        // 等配网凭证 / teardown / 在线断连(共用事件组等待,凭证优先处理)。
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, EV_CREDENTIALS | EV_STOP | EV_DISCONNECT, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(500));

        if (bits & EV_STOP) {
            // F3:teardown/ap_start 只在本任务做(与 sta_online 串行,无竞态)。
            if (s_teardown_req) {
                s_teardown_req = false;
                wifi_teardown();
                xEventGroupClearBits(s_events, EV_STOP | EV_CREDENTIALS
                                              | EV_GOT_IP | EV_DISCONNECT);
                // 只有明确改网意图(reset_wifi)才重开热点;普通离店归于
                // IDLE,重进商店由 begin() 决定 STA 自动重连或重新配网。
                if (s_ap_after_teardown) {
                    s_ap_after_teardown = false;
                    ap_start();
                }
            }
            continue;
        }

        if (bits & EV_CREDENTIALS) {
            char ssid[33], pass[65];
            snprintf(ssid, sizeof(ssid), "%s", s_prov_ssid);
            snprintf(pass, sizeof(pass), "%s", s_prov_pass);
            if (sta_online(ssid, pass) != ESP_OK) {
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

        // 在线断连自动重连(真机 v63:ONLINE 后 ~10s bcn_timeout 断连,旧代码
        // 无任何恢复 → 手机页面瞬间全打不开,直到退出商店重进)。LAN 安装的
        // 整个用户面都架在 STA 链路上,断连必须自愈。限 3 次,失败回落配网页。
        if ((bits & EV_DISCONNECT) && s_was_online && !s_teardown_req
            && s_status.state == SN_STATE_ONLINE) {
            ESP_LOGW(TAG, "STA 在线断连(reason=%d),自动重连…", s_last_disconnect_reason);
            bool ok = false;
            for (int attempt = 1; attempt <= 3 && !s_teardown_req; attempt++) {
                ESP_LOGI(TAG, "重连尝试 %d/3: %s", attempt, s_last_ssid);
                if (sta_online(s_last_ssid, s_last_pass) == ESP_OK) { ok = true; break; }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (!ok && !s_teardown_req) {
                s_was_online = false;
                char fail_buf[32];
                set_state(SN_STATE_ERROR,
                          meta_store_wifi_fail_text(s_last_disconnect_reason, fail_buf, sizeof(fail_buf)));
                ESP_LOGW(TAG, "STA 重连 3 次失败,回落配网页");
                wifi_teardown();
                ap_start();
            } else if (ok) {
                ESP_LOGI(TAG, "STA 重连成功,安装服务继续");
            }
            continue;
        }

        // 配网页:扫描已收进 h_prov_scan(handler 内同步 起扫→等→取)。
        // r8 教训:跨任务接力 + 空结果分支不取记录,残留扫描态既冻结节后
        // 所有扫描,也卡死 STA 连接;收尾与起扫必须在同一处。
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---- 对外 API ----

esp_err_t meta_store_net_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = net_prepare();
    if (err != ESP_OK) return err;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL);
    if (err != ESP_OK) return err;

    const BaseType_t t = xTaskCreate(job_task_main, "store_net", NET_TASK_STACK, NULL,
                                     NET_TASK_PRIO, &s_net_task);
    if (t != pdPASS) return ESP_ERR_NO_MEM;

    s_initialized = true;
    set_state(SN_STATE_IDLE, "");
    return ESP_OK;
}

esp_err_t meta_store_net_begin(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_status.state == SN_STATE_ONLINE
        || s_status.state == SN_STATE_CONNECTING) {
        return ESP_OK;   // 幂等:流程已在跑
    }

    char ssid[33], pass[65];
    if (creds_load(ssid, pass)) {
        // AP_UP 残留(旧版本离店遗留热点)时,有凭证就拆掉热点走 STA,
        // 否则会被幂等分支永远卡在配网页。teardown 由 job 任务串行做。
        if (s_status.state == SN_STATE_AP_UP) {
            s_teardown_req = true;
            xEventGroupSetBits(s_events, EV_STOP);
            for (int i = 0; i < 30 && (s_teardown_req
                 || s_status.state == SN_STATE_AP_UP); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        // 异步连:网络任务只处理 EV_CREDENTIALS,这里直接投递一个"内部连接"消息,
        // 复用同一处理路径(经 s_prov_* 传递凭证)。
        snprintf(s_prov_ssid, sizeof(s_prov_ssid), "%s", ssid);
        snprintf(s_prov_pass, sizeof(s_prov_pass), "%s", pass);
        xEventGroupSetBits(s_events, EV_CREDENTIALS);
        set_state(SN_STATE_CONNECTING, "Connecting to WiFi...");
        return ESP_OK;
    }
    if (s_status.state == SN_STATE_AP_UP) return ESP_OK;   // 已在配网态,幂等
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
    set_state(SN_STATE_IDLE, "");
}

void meta_store_net_poll(meta_store_net_status_t *out)
{
    if (out) *out = s_status;
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
    s_ap_after_teardown = true;   // 改网意图:teardown 后必须重开热点(job 任务执行)
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
