<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# 隔离区:FAP_SCREENSHOT_V1 串口截屏尝试

本目录存放一次未完成的固件截屏功能尝试:主机经 USB-CDC 发送
`FAP_SCREENSHOT_V1\n`,设备回传当前 LVGL 屏幕的 240x320 RGB565 帧
(协议背景见
[serial-screenshot-protocol.zh_CN.md](../../docs/reference/y2lin/serial-screenshot-protocol.zh_CN.md))。

## 为什么这些文件被隔离

- 它们不属于任何正式提交:两个文件均为未跟踪状态,且本仓库只按显式文件
  列表暂存(绝不使用 `git add -A`)。
- `main/CMakeLists.txt` 使用显式 `SRCS` 列表。曾有一版改动把
  `fap_screenshot.c` 写进去(顶替了 `meta_md5.c`),该改动已回滚,因此固件
  构建从不编译这些文件。
- 没有任何已跟踪源码 include `fap_screenshot.h`;在 `main/`、
  `bootloader_components/`、`components/`、`tests/`、`tools/` 下搜索
  `fap_screenshot` / `FAP_SCREENSHOT` 均无结果。

## 如何重启该实验

1. 把 `fap_screenshot.c`、`fap_screenshot.h` 移回 `main/`。
2. 在 `main/CMakeLists.txt` 的 `SRCS` 列表中加回 `"fap_screenshot.c"`。
3. 在 `app_main` 里通过 `fap_screenshot_init()` / `fap_screenshot_task()`
   启动该服务。

在那之前,请勿把本目录放进任何提交。
