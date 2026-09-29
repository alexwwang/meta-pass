// tests/esp_stubs/esp_http_client.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_http_client *esp_http_client_handle_t;

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
} esp_http_client_method_t;

// r10.7:事件机制桩(HTTP_EVENT_ERROR 供 OPEN 死因捕获;IDF 签名对齐)。
typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t   client;
    void                      *user_data;
    void                      *data;   // HTTP_EVENT_ERROR: esp_tls_error_handle_t
    int                        data_len;
} esp_http_client_event_t;

typedef struct {
    const char            *url;
    esp_http_client_method_t method;
    int                    timeout_ms;
    int                    buffer_size;
    const char            *user_agent;         // r10.8:设备 UA(IDF 字段名对齐)
    bool                   disable_auto_redirect; // r10.8:禁自动重定向(IDF 字段名对齐)
    const char            *cert_pem;
    bool                   skip_cert_common_name_check;
    // r10.5:与 IDF 5.x 对齐 —— crt_bundle_attach 是信任锚首选形态
    // (meta_store_api.c 两处使用);桩保持最小签名,真实实现在 esp-tls。
    esp_err_t             (*crt_bundle_attach)(void *conf);
    // r10.7:事件回调(死因捕获)。
    esp_err_t             (*event_handler)(esp_http_client_event_t *evt);
    void                  *user_data;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
// r10.8(BUG-19):IDF 5.x 真实契约 —— 返回 Content-Length(int64) 而非状态码;
// 旧桩声明成 int 曾让 meta_store_api.c 把 CL 当 HTTP 状态码比较(真机必败)。
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
esp_err_t esp_http_client_get_header(esp_http_client_handle_t client, const char *key, char **value);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
