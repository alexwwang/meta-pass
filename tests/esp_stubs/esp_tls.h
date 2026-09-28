// tests/esp_stubs/esp_tls.h —— host 语法门桩(对齐 IDF 5.5.3 esp-tls)。
// 仅覆盖 meta_store_api.c OPEN 死因分类用到的 API。
#pragma once

#include "esp_err.h"
#include "esp_tls_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_tls *esp_tls_error_handle_t;

esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t h,
                                           esp_tls_error_type_t err_type,
                                           int *error_code);

#ifdef __cplusplus
}
#endif
