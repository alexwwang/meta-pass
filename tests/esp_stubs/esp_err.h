#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL       -1
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND   0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT     0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NOT_FINISHED 0x10C
#define ESP_ERR_NOT_ALLOWED  0x10D
#define ESP_ERR_HTTPD_BASE  0xb000
#define ESP_ERR_HTTPD_HANDLERS_FULL (ESP_ERR_HTTPD_BASE + 1)
#define ESP_ERR_HTTPD_HANDLER_EXISTS (ESP_ERR_HTTPD_BASE + 2)
#define ESP_ERR_HTTPD_INVALID_REQ (ESP_ERR_HTTPD_BASE + 3)
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 4)
#define ESP_ERR_HTTPD_RESP_HDR (ESP_ERR_HTTPD_BASE + 5)
#define ESP_ERR_HTTPD_RESP_SEND (ESP_ERR_HTTPD_BASE + 6)
#define ESP_ERR_HTTPD_ALLOC_MEM (ESP_ERR_HTTPD_BASE + 7)
#define ESP_ERR_HTTPD_TASK (ESP_ERR_HTTPD_BASE + 8)
static inline const char *esp_err_to_name(esp_err_t code) {
    (void)code;
    return "unknown error";
}
