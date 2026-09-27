// tests/esp_stubs/freertos/FreeRTOS.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
// 只提供 meta_store_net.c / esp_netif_sntp.h 桩用到的类型与宏;不链接,仅 -fsyntax-only。
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t StackType_t;

#define pdTRUE   ((BaseType_t)1)
#define pdFALSE  ((BaseType_t)0)
#define pdPASS   ((BaseType_t)1)
#define pdFAIL   ((BaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

TickType_t xTaskGetTickCount(void);
