#pragma once
// 必须引用 tag 参数:GCC 对未使用的 static const char *TAG 报
// -Wunused-variable(clang 不报),CI(Linux/gcc)曾因宏吞掉 tag 而失败。
// r10.10:LOGI 输出到 stdout —— 语法门(-fsyntax-only)不执行,但日志宏必须
// 消费全部实参(含格式化表达式),否则宿主裁掉实参后固件里合法的打点代码在
// host 侧报 unused-variable。真实格式化交给固件;host 用 printf 消费即可。
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); printf("I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); printf("W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); printf("E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); (void)0; } while (0)
#define ESP_LOGV(tag, fmt, ...) do { (void)(tag); (void)0; } while (0)
