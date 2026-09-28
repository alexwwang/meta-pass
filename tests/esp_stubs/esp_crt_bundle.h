// tests/esp_stubs/esp_crt_bundle.h —— host 语法桩:IDF esp_crt_bundle 最小签名。
// 真实实现由 esp-tls 组件提供(main/CMakeLists REQUIRES esp-tls);
// 桩只保证 -fsyntax-only 门能解析 meta_store_api.c 的 crt_bundle_attach 引用。
#pragma once

#include <stdint.h>

typedef struct crt_bundle_t *crt_bundle_t;

// IDF 签名:esp_err_t esp_crt_bundle_attach(void *conf);
// 桩里 esp_err_t 已由 esp_err.h 桩定义;参数用 void* 对齐真签名的宽松形态。
esp_err_t esp_crt_bundle_attach(void *conf);
