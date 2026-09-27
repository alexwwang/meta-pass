// tests/esp_stubs/freertos/task.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t prio, TaskHandle_t *out_handle);
void vTaskDelete(TaskHandle_t handle);
void vTaskDelay(TickType_t ticks);
