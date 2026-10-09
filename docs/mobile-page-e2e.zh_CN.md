<p align="right">\n  <strong>简体中文</strong> · <a href="mobile-page-e2e.md">English</a>\n</p>\n\n# 手机端设备内嵌网页 E2E 自动化测试方案

状态：方案已确定，按本文实施。目标分支：`feat/storage`。

## 1. 目标与边界

为 ESP32 设备提供给手机访问的安装网页建立可重复的端到端测试。测试运行在桌面 Chromium，但通过 Playwright 的移动端设备参数模拟手机视口、DPR、触摸能力与移动 User-Agent；网页和 API 仍访问真实设备，安装、删除均由页面 UI 触发。

不把 Android 模拟器、Appium 或原生宿主 App 作为前置条件。只有将来需要验证原生 WebView 桥接、系统权限或容器生命周期时，才单独运行 WebView 专项测试。

参考 `tools/realdevice/usb_web_e2e_multi.mjs` 的核心约束：
- 一个浏览器会话贯穿整轮测试，不在步骤间重启浏览器。
- 所有有副作用的安装/删除都通过页面 UI；测试程序禁止调用安装写 API。
- 通过设备只读 API 独立核验槽位与安装器状态。
- 记录分阶段 PASS/FAIL、截图、页面源码、浏览器 console/page error、脱敏 JSON/文本报告。
- 仅在显式传入 `--real-device` 后允许操作真机；测试只删除本轮创建且名称精确匹配的测试槽位。

## 2. 测试架构

1. Playwright Chromium 启动一次，创建 mobile emulation context（默认 390×844、DPR=3、isMobile、hasTouch、Android Chrome UA）。
2. 打开真实设备根地址；会话 token 从 URL fragment `#s=...` 或环境变量读取，不写入报告/命令记录。
3. 等待设备壳页与 `phone-install.js` 挂载完成，验证页面脚本、视口布局和关键控件。
4. 读取初始槽位和安装器状态作为基线。
5. 用网页搜索框按配置的两个 play ID 搜索玩法、查看详情、打开安装选择面板、选择槽位/名称并点击「确认安装」。测试等待真实设备确认流程和页面上传/完成状态，不绕过物理确认。
6. 每次安装完成后通过只读设备 API 核验新槽位有效、名称匹配、安装器回到 idle；安装第二个玩法后核验两者共存。
7. 通过「空间管理」UI 删除本轮创建的玩法；有归档数据时勾选“同时删除数据”，避免测试数据污染基线。每次删除后独立核验另一槽位仍存在。
8. 最终对比设备槽位/数据基线；失败时仅尝试通过 UI 清理本轮生成的唯一测试名称，无法安全确认目标时不执行删除。

## 3. 覆盖用例

| 编号 | 场景 | 通过条件 |
|---|---|---|
| M01 | 手机视口打开真实设备页面 | HTTP 导航成功、模块挂载、关键控件存在、无横向溢出 |
| M02 | 初始状态/基线读取 | `GET /api/install/slots` 与 `GET /api/install/status` 成功，安装器 idle |
| M03 | 搜索玩法并查看详情 | 输入 ID 搜索后显示详情和安装按钮 |
| M04 | 安装第一个玩法 | 通过 UI 选槽/命名/确认；页面显示完成；只读 API 看到 valid 槽位 |
| M05 | 安装第二个玩法 | 第二个槽位有效，首个槽位仍存在；安装器 idle |
| M06 | UI 删除第一个玩法 | 通过空间管理 UI 两步确认删除；第二个槽位保留 |
| M07 | UI 删除第二个玩法 | 通过空间管理 UI 删除；不残留本轮槽位 |
| M08 | 基线恢复 | 最终设备槽位与数据状态和基线一致 |
| M09 | 故障证据与清理 | 失败时报告、截图、源码可读；仅清理名称精确匹配的本轮测试槽位 |

## 4. 安全和稳定性规则

- 必须显式传入 `--real-device`；缺少参数时在启动浏览器前退出。
- 两个 play ID 必须不同；测试显示名使用唯一短标识，禁止与基线槽位同名。
- 不通过 `/api/install/prepare`、`/session`、`/chunk`、`/finalize`、`/remove` 等写接口绕过 UI。
- 设备 API 只读校验仅访问 `/api/install/slots` 与 `/api/install/status`，携带会话 token；token、URL fragment、IP 不进入报告内容。
- 安装等待时间允许设备物理确认/设备重启；超时应明确失败，不能伪造成功。
- 失败恢复只对本轮生成的精确名称进行 UI 删除；任何歧义都停止清理并在报告中提示人工核查。
- 运行期间不重启 Chromium；仅在整轮结束时关闭 context/browser。

## 5. 运行方式

```bash
node tools/realdevice/mobile_page_e2e.mjs --real-device \
  --url 'http://<device-ip>/#s=<session-token>' \
  --play-a <play-id-a> --play-b <play-id-b>
```

也可通过环境变量提供 `META_PASS_IP`、`META_PASS_SESSION`、`MOBILE_E2E_PLAY_A`、`MOBILE_E2E_PLAY_B`。推荐优先使用环境变量传入 token，URL 只传设备地址；运行器会在导航前把 token 加入 fragment，避免 token 进入命令行参数与报告。

默认在同一个浏览器页面、同一个会话内先检查 `320×720`、`360×800`、`390×844`、`430×932` 四种手机视口是否出现横向溢出，再恢复默认 `390×844` 视口执行安装/删除业务流程。可通过 `--viewports 320x720,375x812,430x932` 自定义矩阵；视口检查阶段不修改设备状态，也不重启浏览器。

本地需要 Node.js、仓库现有 Playwright 依赖及可启动的 Chromium/Chrome。此真机 E2E 不在没有设备的普通 CI 中运行；CI 运行静态契约测试与脚本语法检查。

## 6. 验收标准

- 静态契约测试覆盖移动端仿真配置、UI 驱动、只读 API 边界、完整安装/删除阶段、脱敏和清理逻辑。
- `tools/validate.sh --static` 通过。
- 真机报告明确区分“脚本/契约测试通过”和“真实设备 E2E 通过”。未连接真机运行时不得声称业务链路通过。
- 浏览器会话只启动一次，完整场景期间不重启。
