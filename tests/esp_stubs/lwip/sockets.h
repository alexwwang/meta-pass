// tests/esp_stubs/lwip/sockets.h —— host 语法检查桩:把 lwip BSD socket API
// 映射到宿主系统头(macOS/Linux 均提供同一套 POSIX socket 接口)。
// 仅用于 -fsyntax-only;目标机编译走 IDF lwip 组件的同名头。
#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
