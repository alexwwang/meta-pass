#pragma once
// 必须引用 tag 参数:GCC 对未使用的 static const char *TAG 报
// -Wunused-variable(clang 不报),CI(Linux/gcc)曾因宏吞掉 tag 而失败。
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#define ESP_LOGV(tag, fmt, ...) ((void)(tag))
