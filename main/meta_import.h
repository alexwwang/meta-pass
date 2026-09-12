// main/meta_import.h —— 固件导入的纯逻辑状态机与配对码策略(与 ESP-IDF/LVGL 解耦)。
// 设备侧装配见 main/meta_net.c;状态语义见 docs/assets/meta-pass-design.md §6。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meta_name.h"   // meta_name_max_app_size():导入上限(尾部 4KB 保留给显示名 blob)

#define MI_CODE_LEN        6   // 屏幕显示的一次性配对码位数(数字)
#define MI_MAX_PAIR_FAILS  3   // 连续配对失败上限,达到后锁定为 ERROR(AUTH)

typedef enum {
    MI_IDLE = 0,    // 未进入导入(Import 页未打开)
    MI_AP_UP,       // 热点与 HTTP 已就绪,等待客户端输入配对码
    MI_PAIRED,      // 配对码已通过,允许上传
    MI_RECEIVING,   // 正在接收固件数据(分块流式写槽位)
    MI_VERIFYING,   // 接收完成,校验中
    MI_DONE,        // 成功:槽位已标记可启动
    MI_ERROR,       // 失败:last_error 指明原因;只能由 MI_EV_RESET 离开
} mi_state_t;

typedef enum {
    MI_ERR_NONE = 0,
    MI_ERR_AUTH,       // 配对码连续错误达到上限
    MI_ERR_TOO_LARGE,  // Content-Length 超过槽位容量(在 UPLOAD_BEGIN 前由策略函数拦截)
    MI_ERR_BROKEN,     // 接收中断/超时/对端断开
    MI_ERR_BAD_IMAGE,  // 镜像校验失败
} mi_error_t;

typedef enum {
    MI_EV_AP_READY,      // 热点与 HTTP 服务启动完成
    MI_EV_PAIR_OK,       // 配对码正确
    MI_EV_PAIR_FAIL,     // 配对码错误
    MI_EV_UPLOAD_BEGIN,  // 一次长度合法的上传开始
    MI_EV_UPLOAD_DONE,   // 声明长度已全部写入
    MI_EV_VERIFY_OK,     // 镜像校验通过
    MI_EV_VERIFY_FAIL,   // 镜像校验失败
    MI_EV_ABORT,         // 接收中断/超时/取消
    MI_EV_RESET,         // 离开 Import 页或开始新一轮(任意状态 → IDLE)
} mi_event_t;

typedef struct {
    mi_state_t state;
    mi_error_t last_error;
    int        pair_fails;   // 本轮连续配对失败次数
} mi_ctx_t;

void mi_init(mi_ctx_t *ctx);

// 处理一个事件,返回新状态。非法事件不改变状态。
mi_state_t mi_handle(mi_ctx_t *ctx, mi_event_t ev);

// 由 32 位随机数生成 6 位数字配对码(允许前导零)。out 至少 7 字节。纯函数。
bool mi_code_gen(uint32_t random32, char out[MI_CODE_LEN + 1]);

// 常量时间比较两个配对码;长度不是恰好 MI_CODE_LEN 直接判负。
bool mi_code_equal(const char *a, const char *b);

// Content-Length 策略:必须为正且不超过上限。调用方传 meta_name_max_app_size(part_size)
// (槽位尾部最后 4KB 保留给显示名 blob,应用镜像有效上限 = 分区大小 − 4KB)。
bool mi_content_length_ok(int64_t declared, uint32_t slot_size);
