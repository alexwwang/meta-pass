// tests/esp_stubs/esp_timer.h —— host 语法检查用最小桩(IDF esp_timer 签名对齐)。
// r10.10:meta_store_api.c 下载遥测用 esp_timer_get_time()(单调微秒时钟)。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// IDF 签名:int64_t esp_timer_get_time(void) —— 自 esp_timer_init 起的微秒数。
int64_t esp_timer_get_time(void);

// P0-5:安装完成复位定时器(esp_timer_create/start_once,签名对齐 IDF 5.x)。
typedef struct esp_timer *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *arg);
    void *arg;
    const char *name;
} esp_timer_create_args_t;
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle);
int esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);

#ifdef __cplusplus
}
#endif
