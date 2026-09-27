// tests/esp_stubs/freertos/event_groups.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
#pragma once

#include "FreeRTOS.h"

typedef void *EventGroupHandle_t;
typedef TickType_t EventBits_t;

#define BIT0 ((EventBits_t)1u << 0)
#define BIT1 ((EventBits_t)1u << 1)
#define BIT2 ((EventBits_t)1u << 2)
#define BIT3 ((EventBits_t)1u << 3)
#define BIT4 ((EventBits_t)1u << 4)
#define BIT5 ((EventBits_t)1u << 5)
#define BIT6 ((EventBits_t)1u << 6)
#define BIT7 ((EventBits_t)1u << 7)

EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_all,
                                TickType_t wait);
