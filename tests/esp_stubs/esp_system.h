// host 桩:对齐 IDF 5.x esp_system.h 的最小替身。
// 仅供 tools/validate.sh 的 -fsyntax-only 检查(meta_store_install.c 的重启续连
// 路径);不链接,无实现。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn)) void esp_restart(void);

#ifdef __cplusplus
}
#endif
