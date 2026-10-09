<p align="right">
  <strong>简体中文</strong> · <a href="mobile-embedded-page-e2e.md">English</a>
</p>

# 手机内嵌页面 E2E 自动化测试框架

## 目标

本框架通过真实 Android 自动化会话驱动手机端页面。它与 `usb_web_e2e_multi.mjs` 分离：后者验证桌面 USB/Web Serial 页面，不能作为手机 UI 的测试证据。

## 架构

- Appium + UiAutomator2 负责 Android 设备控制，并启动 Chrome 或指定的原生宿主 App。
- 自动发现可用上下文并切换到真实的 `WEBVIEW_*` / `CHROMIUM` 上下文；不通过缩小桌面 Chrome 视口来模拟手机。
- JSON 场景驱动 UI 操作，支持 `waitVisible`、`click`、`fill`、`waitText`、`assertText`、`assertJs` 和 `sleep`。
- 所有写操作必须通过 UI 完成。允许使用只读的 `GET /api/install/slots` 独立核验设备状态；禁止直接调用安装/删除写 API。
- 失败时保存脱敏页面源码、截图、机器可读 JSON 报告和文本报告。文本证据会脱敏 token 和 IP。

## 环境要求

1. 开启 USB 调试且已授权 ADB 的 Android 手机。
2. 可访问的 Appium 2 服务（默认 `http://127.0.0.1:4723`），已安装 `uiautomator2` driver。
3. 使用 `--mode webview` 时，需要提供承载内嵌页面的 App 包名和启动 Activity；App 必须暴露可调试 WebView。
4. 手机与 ESP32 处于可互通网络。URL 使用固件显示的实际设备地址；若需要 token，运行时传入完整 URL（含 fragment），不要将秘密写进场景文件。
5. 使用 Node.js；框架只使用 Node 内置模块，不增加 npm 依赖。

## 运行

在仓库根目录执行：

```bash
# 原生 App 内嵌 WebView（目标场景）
node tools/realdevice/mobile_webview_e2e.mjs \
  --mode webview --app-package <package.id> --app-activity <activity> \
  --url 'http://<device-ip>/' --ip <device-ip> --scenario tools/realdevice/mobile-webview-scenario.json

# Android Chrome 手机浏览器路径（独立模式，不能替代 WebView 测试）
node tools/realdevice/mobile_webview_e2e.mjs \
  --mode browser --url 'http://<device-ip>/' --ip <device-ip>
```

## 场景定义

仓库默认场景是连通性与页面壳层 smoke：验证手机页面非空，并读取设备槽位清单。由于原生宿主 App 不在本仓库中，不能猜测其包名、Activity 或页面 DOM 选择器。需要根据实际宿主页面补充完整安装流程的 UI 选择器。

支持的动作：

- `waitVisible`：等待元素存在，参数 `selector`，可选 `timeout`
- `click`：点击元素
- `fill`：填写元素，支持固定值或 `deviceIp` / `deviceUrl`
- `waitText` / `assertText`：等待或断言元素文本
- `assertJs`：执行只读页面断言
- `snapshotSlots`：在流程中保存设备槽位快照
- `assertSlotPresent` / `assertSlotAbsent`：对命名快照断言槽位存在性
- `assertInstallerIdle`：核验安装服务未残留活动会话
- `sleep`：等待指定毫秒数

完整的安装→进度→完成→删除→恢复基线测试，需要提供与实际手机内嵌页面一致的场景选择器。默认场景不会冒充完整业务 E2E。

## 覆盖边界

- 框架已实现：Appium 生命周期、启动 Android App/Chrome、真实 WebView 发现与切换、场景驱动 UI、设备状态回读、脱敏证据、退出码和会话清理。
- 默认场景：页面壳层 smoke。
- 完整安装业务链：需要在确认宿主 App 和实际 DOM 之后配置场景。
- 云端静态 CI 没有实体手机/Appium 设备，因此只运行语法与契约测试，不声称真机 E2E 已通过。

证据写入 `tools/realdevice/logs/mobile-e2e-<timestamp>/`，包含 `report.json`、`report.txt`、`device-slots.json`；失败时附加 `page-source.xml` 与 `failure.png`。
