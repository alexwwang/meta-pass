#pragma once
#include "esp_err.h"
esp_err_t fap_screenshot_init(void);
void fap_screenshot_task(void *pvParameters);