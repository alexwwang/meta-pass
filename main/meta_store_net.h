// main/meta_store_net.h —— 商店下载通道的网络管理层:
//   SoftAP 配网页(只收 WiFi 凭证,无配对码) → STA 连接 → SNTP 同步 → 作业队列。
// analyze/install 的 HTTP+OTA 细节在 meta_store_api;本模块只管"让 API 调用可行"
// 以及"在专用网络任务里执行 API 调用",UI 永不直接触碰阻塞网络调用。
//
// 资源纪律沿用 meta_net(docs/reference/phoenixzhc/softap-provisioning-and-resource-budget):
// AP-only、max_connection=1、进出完整启停;凭证存自有 NVS 命名空间(复用上次配网,
// 连接失败自动回落配网页)。
//
// 线程模型:网络任务是唯一调用 meta_store_api_* 的上下文;UI(LVGL/按键任务)通过
// meta_store_net_poll()/meta_store_net_job_poll() 读快照,通过 meta_store_net_cmd_*()
// 投递作业,互不直接调用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "meta_slots.h"
#include "meta_store_api.h"

typedef enum {
    SN_STATE_IDLE = 0,     // 未启动(未进入商店页)
    SN_STATE_AP_UP,        // 配网 SoftAP 就绪,等待网页提交 WiFi 凭证
    SN_STATE_CONNECTING,   // STA 连接中
    SN_STATE_ONLINE,       // 已联网且 SNTP 已同步,可执行 analyze/install
    SN_STATE_ERROR,        // 失败(message 可展示原因)
} sn_state_t;

// 作业(analyze / install)状态。
typedef enum {
    SN_JOB_IDLE = 0,       // 无作业
    SN_JOB_RUNNING,        // 网络任务执行中
    SN_JOB_DONE_OK,        // 成功(analyze 结果可用 / install 已刷写)
    SN_JOB_DONE_FAIL,      // 失败(message 可展示原因)
} sn_job_state_t;

// 网络状态快照(UI 轮询)。
typedef struct {
    sn_state_t state;
    char ssid[20];         // 配网 SoftAP SSID(仅 AP_UP 时有效)
    char password[12];     // 配网 SoftAP WPA2 随机密码(仅 AP_UP 时有效)
    char sta_ssid[33];     // 当前使用的路由器 SSID(ONLINE/CONNECTING 时有效)
    char message[48];      // 英文状态短句,直接上屏
} meta_store_net_status_t;

// 作业状态快照(UI 轮询)。
typedef struct {
    sn_job_state_t state;
    char message[48];      // 失败原因 / 进行提示(INSTALL 进度另见 meta_store_api_poll)
} meta_store_net_job_t;

// 一次性初始化:NVS/netif/event loop/网络任务/作业队列。幂等。失败返回 esp_err_t。
// slots 为启动器槽位注册表(install 成功由 meta_store_api 回写);必须非空且生命周期
// 覆盖整个商店通道(由 app_main 传入启动器的静态注册表)。
esp_err_t meta_store_net_init(meta_slot_info_t slots[META_SLOT_COUNT]);

// 进入商店流程:优先用已存凭证连路由器;失败或无凭证则开配网 SoftAP。
// 返回仅表示"流程已启动";实际结果经 meta_store_net_poll 轮询(ONLINE / AP_UP / ERROR)。
esp_err_t meta_store_net_begin(void);

// 离开商店页时完整释放:配网 httpd → WiFi → 取消未完成作业。可重复调用。
void meta_store_net_stop(void);

// 投递 analyze 作业(要求当前 ONLINE)。ESP_OK = 已入队,结果经作业轮询取。
esp_err_t meta_store_net_cmd_analyze(uint32_t play_id);

// 投递 install 作业(要求当前 ONLINE;slot 必须在 analyze 给出的可装集合内,
// 由 UI 负责约束,此处仅做越界检查)。analysis 由设备在 analyze 作业成功后缓存。
esp_err_t meta_store_net_cmd_install(uint32_t play_id, int slot);

// analyze 成功后的分析结果(作业 DONE_OK 后有效;install 内部也复用同一缓存)。
const meta_store_analysis_t *meta_store_net_analysis(void);

void meta_store_net_poll(meta_store_net_status_t *out);
void meta_store_net_job_poll(meta_store_net_job_t *out);

// ---- 商店会话超时配置 ----
// 默认值 = CONFIG_META_STORE_SESSION_TIMEOUT_MS(main/Kconfig.projbuild,menuconfig 可改);
// 运行时可用 setter 覆盖(如后续做设置页):有效范围钳制在 30s..24h。
// 超时语义:到期不强制关闭,由 UI 提示用户选择"保留 / 退出"(见 main.c store_tick)。
void     meta_store_session_set_timeout_ms(uint32_t ms);
uint32_t meta_store_session_timeout_ms(void);
