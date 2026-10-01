// main/meta_store_net.h —— 商店流程的网络管理层(feat/mota 净切后):
//   SoftAP 配网页(扫描点选 + 手输,无配对码)→ STA 连接 → 在线态。
//   配网热点为开放网络 + DNS 劫持(Captive Portal 自动弹配置页;302 兜底),
//   APSTA 后台周期扫 AP 供 /api/scan 点选;凭证到手即关热点转 STA。
// 旧的 WAN analyze/install 作业链已随 feat/mota 移除:设备不再经 TLS 出网,
// 安装走本地 LAN install 服务(meta_store_install);本模块只负责"让设备上线"。
//
// 资源纪律沿用 meta_net(docs/reference/phoenixzhc/softap-provisioning-and-resource-budget):
// 进出完整启停;凭证存自有 NVS 命名空间(复用上次配网,连接失败自动回落配网页)。
//
// 线程模型:网络任务是唯一碰 WiFi 启停的上下文;UI(LVGL/按键任务)通过
// meta_store_net_poll() 读快照、经 meta_store_net_reset_wifi() 投递改网请求,
// 互不直接调用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SN_STATE_IDLE = 0,     // 未启动(未进入商店页)
    SN_STATE_AP_UP,        // 配网 SoftAP 就绪,等待网页提交 WiFi 凭证
    SN_STATE_CONNECTING,   // STA 连接中
    SN_STATE_ONLINE,       // 已联网(STA 拿到 IP),可启动本地 LAN install 服务
    SN_STATE_ERROR,        // 失败(message 可展示原因)
} sn_state_t;

// 网络状态快照(UI 轮询)。
typedef struct {
    sn_state_t state;
    char ssid[20];         // 配网 SoftAP SSID(仅 AP_UP 时有效;热点已无密码)
    char sta_ssid[33];     // 当前使用的路由器 SSID(ONLINE/CONNECTING 时有效)
    char message[48];      // 英文状态短句,直接上屏
} meta_store_net_status_t;

// 一次性初始化:NVS/netif/event loop/网络任务。幂等。失败返回 esp_err_t。
esp_err_t meta_store_net_init(void);

// 进入商店流程:优先用已存凭证连路由器;失败或无凭证则开配网 SoftAP。
// 返回仅表示"流程已启动";实际结果经 meta_store_net_poll 轮询(ONLINE / AP_UP / ERROR)。
esp_err_t meta_store_net_begin(void);

// 离开商店页时完整释放:配网 httpd → WiFi。可重复调用。
void meta_store_net_stop(void);

// 清除已存 WiFi 凭证并重新开配网 AP(改 WiFi 入口)。要求已初始化;内部
// 完成旧连接 teardown → 凭证擦除 → ap_start,状态经 meta_store_net_poll 反映。
// 返回 ESP_OK 表示 AP 已就绪(或已在配网态);错误时状态为 ERROR + message。
esp_err_t meta_store_net_reset_wifi(void);

void meta_store_net_poll(meta_store_net_status_t *out);

// ---- 商店会话超时配置 ----
// 默认值 = CONFIG_META_STORE_SESSION_TIMEOUT_MS(main/Kconfig.projbuild,menuconfig 可改);
// 运行时可用 setter 覆盖(如后续做设置页):有效范围钳制在 30s..24h。
// 超时语义:到期不强制关闭,由 UI 提示用户选择"保留 / 退出"(见 main.c store_tick)。
void     meta_store_session_set_timeout_ms(uint32_t ms);
uint32_t meta_store_session_timeout_ms(void);
