#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_event.h"

typedef void *httpd_handle_t;
enum http_method { HTTP_GET = 0, HTTP_POST = 1, HTTP_PUT = 2, HTTP_DELETE = 3, HTTP_ANY = 0x7fffffff };
typedef enum http_method httpd_method_t;
typedef struct httpd_req httpd_req_t;
typedef struct httpd_config httpd_config_t;
typedef esp_err_t (*httpd_open_func_t)(httpd_handle_t hd, int sockfd);
typedef void (*httpd_close_func_t)(httpd_handle_t hd, int sockfd);
typedef bool (*httpd_uri_match_func_t)(const char *reference_uri, const char *uri_to_match,
                                        size_t match_upto);
typedef esp_err_t (*httpd_send_func_t)(httpd_handle_t hd, int sockfd, const char *buf, size_t len,
                                        int flags);
typedef int (*httpd_pending_func_t)(httpd_handle_t hd, int sockfd);
typedef void (*httpd_free_ctx_fn_t)(void *ctx);

#define HTTPD_RESP_USE_STRLEN -1

/* httpd_req_t:meta_net.c 读取 content_len;测试通过 recv_buf/recv_offset 注入接收数据,
 * 通过 resp_status/resp_body 捕获响应。 */
struct httpd_req {
    httpd_handle_t handle;
    int method;
    char uri[512];
    size_t content_len;
    void *aux;
    void *user_ctx;
    void *sess_ctx;
    /* 测试注入:recv 数据源与控制 */
    const char *recv_buf;
    size_t recv_buf_len;
    size_t recv_offset;
    size_t recv_chunk_size;    /* 0=不限制;否则每次 recv 最多返回这么多字节 */
    int recv_fail_at_call;     /* 0=不注入;N=第 N 次 recv 调用返回 0(1-based) */
    int recv_call_count;
    /* 响应捕获 */
    char resp_status[64];
    char resp_body[4096];
    int resp_body_len;
};

typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;
} httpd_uri_t;

struct httpd_config {
    unsigned task_priority;
    size_t stack_size;
    int core_id;
    uint32_t task_caps;
    size_t max_req_hdr_len;
    size_t max_uri_len;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    void *global_user_ctx;
    httpd_free_ctx_fn_t global_user_ctx_free_fn;
    void *global_transport_ctx;
    httpd_free_ctx_fn_t global_transport_ctx_free_fn;
    bool enable_so_linger;
    int linger_timeout;
    bool keep_alive_enable;
    int keep_alive_idle;
    int keep_alive_interval;
    int keep_alive_count;
    httpd_open_func_t open_fn;
    httpd_close_func_t close_fn;
    httpd_uri_match_func_t uri_match_fn;
};
#define HTTPD_DEFAULT_CONFIG() {0}

/* 以下函数由 meta_net_start/stop 调用,测试不触达,提供空实现即可 */
static inline esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
    (void)config; *handle = (httpd_handle_t)1; return ESP_OK;
}
static inline esp_err_t httpd_stop(httpd_handle_t handle) {
    (void)handle; return ESP_OK;
}
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t hd, const httpd_uri_t *uri) {
    (void)hd; (void)uri; return ESP_OK;
}
static inline esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
    (void)r; (void)type; return ESP_OK;
}
static inline esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, size_t len) {
    (void)r; (void)buf; (void)len; return ESP_OK;
}

/* 以下函数由 h_upload/h_session 直接调用,在测试文件中定义(非 inline) */
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
