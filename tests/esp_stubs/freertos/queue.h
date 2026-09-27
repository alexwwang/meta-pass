// tests/esp_stubs/freertos/queue.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
#pragma once

#include "FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t wait);
BaseType_t xQueueReset(QueueHandle_t queue);
