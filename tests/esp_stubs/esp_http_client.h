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

typedef struct {
    const char            *url;
    esp_http_client_method_t method;
    int                    timeout_ms;
    int                    buffer_size;
    const char            *cert_pem;
    bool                   skip_cert_common_name_check;
    void                  *user_data;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
esp_err_t esp_http_client_get_header(esp_http_client_handle_t client, const char *key, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
