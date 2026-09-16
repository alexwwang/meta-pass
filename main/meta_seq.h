// main/meta_seq.h —— 隐藏按键序列匹配器(纯逻辑,与 BSP/ESP-IDF 解耦)。
// 彩蛋页入口:详情页快速连按 UP UP DOWN DOWN(四个 CLICK),
// 相邻两键间隔必须严格小于 META_SEQ_GAP_MS,否则进度作废。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define META_SEQ_GAP_MS 500u   // 相邻按键允许的最大间隔(不含)

typedef enum {
    META_SEQ_KEY_UP = 0,
    META_SEQ_KEY_DOWN,
} meta_seq_key_t;

typedef struct {
    uint8_t  index;      // 已匹配前缀长度 0..META_SEQ_LEN
    uint32_t last_ms;    // 上一有效键时刻(毫秒,时基由调用方注入,便于测试)
} meta_seq_state_t;

#define META_SEQ_LEN 4u

void meta_seq_reset(meta_seq_state_t *st);

// 喂入一个按键事件;完整匹配序列返回 true(内部状态同时复位,可立即开始下一轮)。
// 间隔超时或键不匹配 → 进度作废,并用当前键尝试作为新序列首键(处理重叠前缀)。
bool meta_seq_feed(meta_seq_state_t *st, meta_seq_key_t key, uint32_t now_ms);
