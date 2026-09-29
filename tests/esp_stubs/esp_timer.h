// tests/esp_stubs/esp_timer.h —— host 语法检查用最小桩(IDF esp_timer 签名对齐)。
// r10.10:meta_store_api.c 下载遥测用 esp_timer_get_time()(单调微秒时钟)。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// IDF 签名:int64_t esp_timer_get_time(void) —— 自 esp_timer_init 起的微秒数。
int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
