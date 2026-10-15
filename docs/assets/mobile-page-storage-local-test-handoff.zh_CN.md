# Handoff：feat/storage 本地验证与真机测试

简体中文 | [English](mobile-page-storage-local-test-handoff.md)

日期：2026-10-11  
仓库：`alexwwang/meta-pass`  
分支：`feat/storage`  
交接基线：`8259c37dbe70bf2fef3593ca750ac9cdc09c8033`  
PR：[#1](https://github.com/alexwwang/meta-pass/pull/1)

## 1. 目标与当前状态

本地 Agent 接手后，目标是完成已有自动化测试框架下的验证，并在前置条件齐备时执行受控真机测试，验证动态 DATA 分区管理及 APP/DATA 生命周期。

**不要另建一套 USB 或手机页面测试框架。** 仓库已经有 USB Web E2E、手机页面 viewport smoke、手机 WebView E2E 和存储管理 E2E。本次工作应复用并补充这些测试。

交接时已知状态：

- GitHub Actions Static checks：commit `8259c37d` 通过。
- GitHub Actions Firmware checks：commit `8259c37d` 通过。
- 这两项不是本地真机 E2E 证据。
- 尚无证据证明本轮已在真机上跑通存储管理 E2E。
- 不要合并 PR，也不要在 `main` 上直接开发。

## 2. 首先同步并检查工作区

在本地仓库根目录执行：

```bash
git fetch origin
git switch feat/storage
git status --short --branch
git log -1 --oneline
```

确认分支基于最新远端 `feat/storage`，并检查本地是否有未提交改动。不要覆盖或丢弃用户的本地改动。若远端已前进，先审查差异再快进/同步；不要强推，不要重置用户工作区。

## 3. 复用的现有测试入口

| 用途 | 现有入口 | 说明 |
|---|---|---|
| 仓库静态检查、契约测试、脚本语法检查 | `./tools/validate.sh --static` | 首先执行；失败时先修复，不要进入破坏性真机测试 |
| 手机页面多视口、非破坏性 smoke | `tools/realdevice/mobile_page_browser_smoke.mjs` | 使用现有 Chromium CDP；不点击写操作、不调用写 API |
| 手机页面存储管理端到端 | `tools/realdevice/mobile_page_e2e.mjs` | 本次 DATA/APP 生命周期的主真机路径；会安装、启动、卸载测试 APP 并写入测试 DATA |
| 手机浏览器/WebView 场景 | `tools/realdevice/mobile_webview_e2e.mjs` | 复用已有场景文件 `tools/realdevice/mobile-webview-scenario.json`；仅在目标入口/宿主 WebView 需要覆盖时执行 |
| USB 网页安装流程 | `tools/realdevice/usb_web_e2e.mjs` | 复用现有 Web Serial 流程；会真实操作设备 |
| USB 多轮安装/卸载与重启验证 | `tools/realdevice/usb_web_e2e_multi.mjs` | 适用于验证 USB 安装、共存、删除及重启后持久性；会真实修改设备 |

相关已有契约测试由 `tools/validate.sh --static` 调用，包括：

- `tests/test_usb_web_e2e_contract.py`
- `tests/test_mobile_webview_e2e_contract.py`
- `tests/test_mobile_page_e2e_contract.py`
- `tests/test_mobile_page_browser_smoke_contract.py`
- `tests/test_mobile_page_storage_management_contract.py`

## 4. 执行顺序

### 阶段 A：本地非破坏性检查

1. 检查 Node.js、Python 版本及工作区状态。
2. 执行 `./tools/validate.sh --static`。
3. 确认新增/修改的自动化脚本均通过语法和契约检查。
4. 启动测试页面服务，并使用本机 Chromium/Chrome CDP 执行手机页面 smoke。

示例（按本机服务实际端口和 URL 调整）：

```bash
node tools/realdevice/mobile_page_browser_smoke.mjs \
  --url 'http://127.0.0.1:4191/m' \
  --cdp-url 'http://127.0.0.1:9222'
```

`/m` 是本地 dev 服务器的手机模块壳页（仅 `META_PASS_DEV=1`；镜像固件 SHELL_HTML，同源加载 `/phone-install.js` 挂载 `#mp-install-root`）。不要对 USB 安装页（`/?mock=1`）跑本 smoke —— 该页不挂载手机模块，关键控件检查必失败。

如果本机服务需要其他启动参数，先查看项目已有启动脚本及说明，不要臆造新的服务方式。Smoke 默认覆盖 320×720、360×800、390×844、430×932 等视口。检查报告里的关键控件、横向溢出、页面错误、控制台错误及请求失败。

### 阶段 B：准备真机环境

开始任何写操作之前，逐项确认：

- 使用专用测试设备，设备中没有需要保留的用户数据。
- 设备运行启用了 `CONFIG_META_E2E_TEST_CONTROL=y` 的 launcher 固件。
- 管理网页可访问，安装器空闲，基线槽表和 DATA reservation 合法。
- 已准备两个不同的、专用于测试的数字 play ID；其对应固件支持 DATA 测试协议。
- 已安装 Node.js（建议 22+）、Python 3 和 `pyserial`。
- 本机 Chromium/Chrome CDP 只监听回环地址，不向局域网开放。
- 已识别正确的 launcher USB Serial/JTAG 串口。
- session token 通过交互方式放入环境变量，不写进命令行参数、日志或提交内容。
- 已阅读报告位置和失败恢复规则。

准备环境示例：

```bash
python3 -m pip install pyserial

# 示例命令；按本机环境调整浏览器可执行文件及 profile 路径。
google-chrome --remote-debugging-port=9222 \
  --user-data-dir=/tmp/meta-pass-cdp

export MOBILE_E2E_URL='http://<device-host>/'
read -rsp 'Session token (32 hex): ' META_PASS_SESSION; echo
export META_PASS_SESSION
export MOBILE_E2E_CDP_URL='http://127.0.0.1:9222'
export META_PASS_E2E_SERIAL_PORT='/dev/<launcher-usb-serial-device>'
```

不要把真实 token、设备 IP、完整 URL 或敏感主机路径粘贴到 PR、issue 或共享日志中。

### 阶段 C：运行手机页面存储管理 E2E

仅在阶段 A/B 均通过后，从仓库根目录执行：

```bash
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device \
  --runtime-driver tools/realdevice/mobile_page_runtime_driver.py \
  --require-data-reservation \
  --play-a <dedicated-test-play-id-a> \
  --play-b <dedicated-test-play-id-b>
```

两个 play ID 必须由执行者显式提供且互不相同；不得使用默认 ID，不得选用包含用户数据的真实应用。该测试会真实安装/卸载测试 APP，并写入测试 DATA，不是只读 smoke。

重点核对以下路径与独立后置条件：

1. 安装器基线空闲，动态槽位几何、对齐、重叠和 free-space 计算正确。
2. APP A 安装成功，设备 API 独立读回其 VALID 槽位。
3. 取消删除不会改变槽表、DATA reservation 或安装器状态。
4. 子固件实际启动，DATA 擦除、写入、读回、checksum 和重启持久性有串口证据。
5. APP B 安装后与 A 共存，二者 APP 槽及 DATA 物理 extent 不重叠。
6. 删除 A 后，A 的 APP/DATA 资源释放，B 仍然有效并能运行。
7. 用 A 的 play ID 以新随机名称 C 重新安装，确认 C 获得独立槽位和新的 DATA reservation；卸载 C 后确认新资源全部释放。
8. 删除 B 后，B 的 APP/DATA 资源释放，设备状态恢复到测试前基线。
9. 无论通过还是失败，都独立读取最终槽表、DATA reservations、installer status 和动态分区几何。

报告默认位于：

`tools/realdevice/logs/mobile-page-e2e-<timestamp>/report.json`

最终成功条件是报告 `verdict=PASS`，并且所有设备运行证据和后置条件都成立。仅有 UI 成功提示、静态契约测试通过或固件构建成功都不算真机 E2E 通过。

### 阶段 D：必要时复用 USB E2E

如果本次改动影响 USB 安装页面、共用安装器协议或 APP 安装/删除的共同行为，继续运行既有 USB E2E；不要复制代码新建另一套 harness。

执行前必须阅读脚本当前 usage 和安全门禁。特别注意：USB 脚本中的默认 play ID 不适合作为真实测试目标，执行时显式提供专用测试 ID，并确认目标设备、IP、session、串口端口均正确。只在专用设备上执行。USB 多轮脚本会修改设备并执行重启/持久性检查。

如果本轮仅验证手机页面专属的 DATA reservation/分区管理行为，先完成阶段 C；不要为了“覆盖更多”而无目的地重复烧写或卸载设备内容。

## 5. 失败与设备恢复规则

- 任一安全门禁失败，停止进一步破坏性设备操作。
- 安装器在安装后没有回到 idle，不得继续后续设备写操作。
- 无法验证 A/B DATA 物理隔离时，停止后续写操作并执行安全清理/恢复流程。
- 失败后只清理本轮随机名称创建的 APP；不得删除测试前基线中已有的 APP。
- 检查报告中的 `deviceRecovery.status`：
  - `RESTORED`：最终设备状态已独立读回并与基线一致。
  - `NOT_RESTORED`：检测到设备未恢复。
  - `UNKNOWN`：无法可靠确认恢复状态。
- 只有 `RESTORED` 才能声称设备已验证恢复。若是 `NOT_RESTORED` 或 `UNKNOWN`，停止重跑，先人工检查设备和恢复快照。
- 保留脱敏报告、失败阶段、关键串口证据和复现步骤。不要为了得到 PASS 而跳过断言、降低验证标准或修改测试结果。

## 6. 交付回报

完成后向项目负责人回报：

1. 实际执行的命令和环境（不包含秘密/token）。
2. 每项测试的 PASS/FAIL/SKIP，以及未执行原因。
3. 报告目录及可分享的脱敏报告/证据。
4. 是否验证了 DATA 的真实写入、读回、checksum 和重启持久性。
5. 最终 `deviceRecovery.status` 和基线对比结果。
6. 发现的问题、涉及文件、修复 commit。
7. 最新分支 SHA 和 CI 状态。

不得把未运行的测试标成 PASS，不得把模拟页面 smoke 说成真机验证，不得合并 PR；所有代码修复继续在 `feat/storage` 分支进行。
