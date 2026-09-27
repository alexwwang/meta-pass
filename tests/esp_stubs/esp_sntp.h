// tests/esp_stubs/esp_sntp.h —— host 语法检查用最小桩(IDF 5.x 签名对齐)。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void esp_sntp_setoperatingmode(unsigned char operating_mode);
void esp_sntp_setservername(unsigned char idx, const char *server);
void esp_sntp_init(void);
void esp_sntp_stop(void);

#ifdef __cplusplus
}
#endif
