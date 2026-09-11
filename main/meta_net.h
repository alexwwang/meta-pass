// main/meta_net.h —— SoftAP + HTTP 固件导入通道(ESP-IDF 依赖集中在此)。
// 协议与信任模型见 docs/assets/meta-pass-design.md §6/§7。
// 线程模型:HTTP handler 运行在 httpd 任务,只写 s_status 普通字段;
// UI 在 LVGL 上下文用 meta_net_poll() 轮询快照,互不直接调用对方 API。
#pragma once

#include "esp_err.h"

#include "meta_import.h"
#include "meta_slots.h"

// UI 轮询用的状态快照(纯数据,拷贝出模块)。
typedef struct {
    char ssid[20];                 // metapass-XXXX
    char password[12];             // WPA2 随机密码(8 字符)
    char code[MI_CODE_LEN + 1];    // 6 位一次性配对码
    mi_state_t state;              // 导入状态机镜像
    mi_error_t error;
    int  progress_pct;             // 0..100;-1 = 当前无上传
    char message[48];              // 英文状态短句(直接显示在屏上)
} meta_net_status_t;

// 启动 SoftAP 与 HTTP 服务。slots 指向启动器持有的槽位注册表,上传成功后回写。
// 幂等:重复调用先 stop 再 start。失败返回 esp_err_t 且保持未启动状态。
esp_err_t meta_net_start(meta_slot_info_t slots[META_SLOT_COUNT]);

// 完整停止并释放:httpd → wifi → netif。可重复调用。
void meta_net_stop(void);

// 取状态快照(LVGL/任意任务上下文安全;读的是 httpd 任务写的普通字段,
// 字段均为字宽内原子或短字符串,UI 允许读到短暂过渡态)。
void meta_net_poll(meta_net_status_t *out);
