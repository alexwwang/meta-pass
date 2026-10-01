// tests/esp_stubs/freertos/semphr.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
// 只提供 meta_store_install.c 会话互斥锁用到的 API;不链接,仅 -fsyntax-only。
#pragma once

#include "FreeRTOS.h"

// IDF 5.x:信号量句柄即队列句柄;host 桩只需不透明指针。
typedef void *SemaphoreHandle_t;

#define portMAX_DELAY ((TickType_t)0xffffffffUL)

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
