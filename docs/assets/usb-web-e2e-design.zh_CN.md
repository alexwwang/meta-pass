# USB Web UI 真机 E2E 测试设计

[English](usb-web-e2e-design.md) | 简体中文

日期：2026-10-08  
分支：`feat/storage`  
状态：设计审查通过后实施

## 1. 背景

当前存储链路已经有两层真机验证：

- `tools/realdevice/smoke.py`：直接调用设备 HTTP API，验证固件存储契约；
- `tools/realdevice/browser_smoke.mjs`：直接调用仓库真实的 `phone-install.js`，验证前端安装模块 × 真机 HTTP API，并已覆盖 APP/DATA。

但二者都没有经过真正的 USB 服务页面，因此仍缺一层：

> **USB Web UI → Chromium Web Serial → esptool-js → ESP32-C3 → Flash**

本测试补的是这一层，而不是重新验证 `phone-install.js`。

仓库当前没有独立的 Playwright/Puppeteer 浏览器 runner；USB 页面本身已经具备真实实现、mock 设备、`server.mjs` 和 `window.__installSlotDebug` 调试桥。因此本方案采用“**现有页面 + 薄 E2E runner**”方式，不复制页面逻辑、不绕过 UI、不建立第二套安装协议。

## 2. 目标

### P0：必须证明

1. Chromium 真正打开当前 `install-slot/install-slot.html`；
2. 使用真实 Web Serial 连接 ESP32-C3；
3. 页面通过真实 `esptool-js` 读取分区表 / carve / slot；
4. UI 完成玩法选择；
5. UI 完成真实玩法元数据获取、固件下载、SHA-256 校验、APP 镜像提取；
6. UI 选择/确定动态槽位；
7. UI 点击 Install；
8. 实际 Flash 写入由页面自己的 loader 完成；
9. UI 最终显示安装成功；
10. 通过真实设备 API 复核 `/api/install/status` 和 `/api/install/slots`；
11. 复核新增槽位的 offset / size / imageLen / state；
12. 测试结束后只清理本测试创建的槽位，并证明设备恢复到测试前状态。

### P1：证据要求

测试必须保存页面截图、浏览器 console / page error、页面安装日志、安装前后的 `/api/install/slots`、安装后的 `/api/install/status`，以及 git SHA、固件 SHA、设备、浏览器、play 等元数据，并生成 JSON + Markdown 报告。

## 3. 明确不做的事情

### 3.1 不直接调用安装模块

测试代码不得直接调用 `runInstall()`、`prepareImage()`、`createBridge().prepare()` 或 `POST /api/install/*`。这些由 `browser_smoke.mjs` 覆盖。

真机 E2E 必须从页面按钮进入真实安装流程。

### 3.2 不 mock Web Serial

不得替换 `navigator.serial`、`Transport`、`ESPLoader` 或 `loader.readFlash/writeFlash`，不得注入 fake loader。

唯一允许的浏览器自动化辅助是选择**已经授权的真实串口设备**。

### 3.3 不测试 DATA

本轮只做 APP-only。Child DATA 已由当前真机 `browser_smoke.mjs` P2 覆盖：data carve、DATA upload/resume/done、finalize 和 Flash byte compare。USB UI E2E 不重复这一层。

## 4. 总体架构

```
existing USB page
      │
install-slot.html
      │
      ├── normal/mock → existing tests
      │
      └── real E2E → thin browser runner
                           │
                       Chromium
                           │
                      Web Serial API
                           │
                        esptool-js
                           │
                        ESP32-C3
                           │
                      Flash / store
                           │
                  /api/install/status
                  /api/install/slots
```

已有 mock 模式保持不变；real 模式只在显式参数下运行。

## 5. 浏览器自动化方案

仓库目前没有浏览器自动化依赖，因此新增一个最小的 Chromium E2E runner；它只负责浏览器动作、等待、断言和证据，不复制页面安装逻辑。

原则：

- 只允许 Chromium；
- headed 模式；
- 使用独立浏览器 profile；
- 使用仓库 `tools/install-slot/server.mjs`；
- 不访问线上部署页面；
- 默认不执行；
- 必须显式 `--real-device` 才触碰硬件。

### 5.1 Web Serial 权限

Web Serial 的设备选择器不能被测试代码伪造。

采用一次性设备授权：

1. 使用同一个 E2E Chrome profile；
2. 首次运行前人工打开 `http://localhost:4191/`；
3. 点击 Connect；
4. 选择目标 ESP32-C3；
5. 允许该 origin 使用该串口；
6. 之后 E2E 使用同一 profile。

为避免自动化再次弹系统 chooser，页面增加极小的测试接入点：

`?e2e=real` 时，Connect handler：

- 仍使用 `navigator.serial`；
- 从 `navigator.serial.getPorts()` 取得已经授权的真实 port；
- 若没有恰好一个候选，测试立即失败并给出授权提示；
- 不创建 fake port；
- 后续仍走原有 `Transport → ESPLoader → loader` 路径。

因此改变的是“真实 port 如何被选择”，不改变“如何连接、如何读写 Flash”。

## 6. 页面可测试性改造

只允许增加两个测试辅助点。

### 6.1 Real E2E 连接模式

增加：

```
?e2e=real
```

只影响 Connect 的 port selection：

```js
const port = E2E_REAL
  ? await getExactlyOneAuthorizedSerialPort()
  : await navigator.serial.requestPort();
```

其余连接代码保持原样。

### 6.2 调试桥

继续使用现有：

```js
window.__installSlotDebug
```

只读暴露 loader、slotModel、slotView、page build。

测试可以通过它做页面状态断言，但**不得通过 debug bridge 直接调用安装函数或写 Flash**。

## 7. 测试对象选择

优先使用 Community Play：

```
Community play
→ Fetch
→ Install
```

而不是测试代码自己下载镜像。

这样可以覆盖页面自己的 play metadata 请求、固件下载、size check、SHA-256、`extractAppImage()`、display name 和最终安装。

默认 play 使用现有真机 browser smoke 已验证、容量可接受的 APP-only play；允许参数覆盖。若默认 play 后端不可用，测试必须 FAIL，而不是自动切换本地 fixture，以避免“下载链路没测到却报告 PASS”。

## 8. 测试流程

### E0 — 环境门禁

检查：

- 显式 real-device 开关；
- Chromium 可用；
- Web Serial API 存在；
- 本地 server 可启动；
- E2E profile 可用；
- 已授权 serial port 恰好一个；
- 目标设备为 ESP32-C3；
- 设备 HTTP API 可访问；
- `/api/install/slots` 可读。

任何门禁失败直接 FAIL，不修改 Flash。

### E1 — 记录 baseline

通过设备真实 HTTP API 读取 `GET /api/install/slots` 和 `GET /api/install/status`，保存 baseline。

重点记录 APP slots、DATA records、offset、size、play_id、state、image_len、name。

### E2 — 打开真实 USB 页面

访问：

```
http://localhost:4191/?e2e=real
```

断言页面脚本成功、`__installSlotReady === true)、无 module error、无 pageerror、不是 mock 模式，且 page build 可追溯。

### E3 — Connect

点击真实 `#btn-connect`。

断言 chip status = connected、显示 ESP32-C3、出现真实连接日志、slot rows 出现、slot model 已 loaded。

必须实际发生：

```
navigator.serial
  → Transport
  → ESPLoader.main()
  → readFlash()
```

### E4 — UI 读取真实槽位

从页面 UI / `__installSlotDebug.slotView` 断言槽位数量、offset、size、state、APP/DATA 分类、summary，再与 baseline `/api/install/slots` 对拍。

### E5 — 选择 Community Play

点击 Community play，输入 play id，点击 Fetch。

断言 play metadata、firmware size、firmware SHA-256 出现且 Install button 可用。

测试不直接调用 `/api/play`。

### E6 — Install

点击 `#btn-install`。

页面内部必须实际完成：

```
download → size check → SHA-256 → extractAppImage
→ slot target selection → dynamic carve/reuse
→ record → partition table → write APP → metadata → done
```

测试只观察 UI，不替页面执行任何一步。

### E7 — 安装完成断言

等待 `#install-status` 进入成功状态，并断言页面日志至少包含 download、sha256 verified、extracted、writing slot、install done，以及 dynslot 下的 record written / created/overwritten。

### E8 — 真实设备复核

独立读取 `GET /api/install/status` 和 `GET /api/install/slots`。

断言 protocol 正确，HTTP 安装会话保持 idle（USB 页面走 Web Serial/esptool-js，不应伪装成 HTTP 安装）；新 slot 出现，offset/size/image_len 有效，state = VALID、name 与 UI 一致，且没有覆盖 baseline 已有 slot。

这是第二条证据链：

```
UI PASS + device HTTP PASS
```

不能只看 UI 文案。

### E9 — Flash 结果检查

最小真实性要求：

- UI 使用真实 `loader.writeFlash`；
- device API 看到新 carve；
- 新 APP slot 的 `image_len` 正确；
- 重新读取后 slot 仍存在。

不在 USB E2E 中重复整槽 byte compare；现有 `browser_smoke.mjs` 已覆盖真实 Flash byte-level / DATA integrity。

### E10 — Cleanup

所有路径进入 cleanup。

只删除本测试创建的 slot，并使用：

```
offset + size + play/name/test marker
```

做身份确认，禁止误删已有玩法。

优先通过现有 UI Remove 清理；如果安装中途失败导致页面 loader 不可用，再使用设备 API 做 recovery cleanup。Recovery cleanup 不作为 UI E2E PASS 的依据。

### E11 — 恢复验证

重新读取 `GET /api/install/slots`，要求资源状态与 baseline 等价。

比较 APP slot 数、offset/size/state/imageLen/playId/name 和 DATA records。允许内部 seq 变化，但不能留下测试创建的资源。

## 8.1 Analyze 是逻辑断言，不是独立 UI 步骤

本测试**不增加 Analyze 按钮，也不要求操作者在每个动作前手动点击 Analyze**。测试的目标是验证真实产品流程，而不是为了测试制造额外 UI 流程。

“Analyze”表示对当前动作前后的状态做结构化检查，并附着在真实动作上：

| 发生位置 | 逻辑分析内容 | 失败时的行为 |
|---|---|---|
| Connect 后（E3/E4） | 协议/芯片识别、dynslot 模式、slot model 已加载；UI 槽位与设备 API baseline 对齐 | 停止安装，不写 Flash |
| Fetch 后、Install 前（E5/E6） | play 元数据、镜像大小与 SHA-256 可见；安装目标唯一；本 E2E 只选择 dynslot Auto（其启用状态证明容量规划可行），不复用 baseline 中已有的空槽，也不覆盖已有槽 | 停止安装，不写 Flash |
| Install 后（E7–E9） | UI 成功状态、下载校验/提取/写入日志；设备 API 中出现唯一新增 VALID APP 槽，imageLen/offset/size 合理 | 进入受身份约束的清理流程，并判定失败 |
| Remove 前后（E10/E11） | 删除目标必须与本次运行登记的 offset、size、name 一致；删除后资源集合与 baseline 等价 | 不确定归属时禁止删除；恢复失败则整体 FAIL |

这些分析不应被报告成独立的 UI 操作阶段。报告仍保留 E0–E11 真实用户动作/验证阶段，但可在相应阶段的 check 名称和 detail 中记录分析结果。测试不得通过调试桥替页面做安装决策或写 Flash。

## 9. 失败与清理策略

采用：

```
try {
  baseline
  browser E2E
} finally {
  cleanup()
  verifyBaseline()
}
```

原则：

1. cleanup 不依赖某个中间步骤必然成功；
2. 检测到测试创建槽位立即登记；
3. 页面关闭前主动 disconnect；
4. browser 崩溃后 runner 仍执行 recovery cleanup；
5. cleanup 失败则最终 FAIL，即使主安装 PASS；
6. 无法证明对象属于本测试时禁止删除。

## 10. 报告

目录：

```
tools/realdevice/logs/usb-web-e2e-YYYYMMDD-HHMMSS/
```

至少保存：

```
command.txt
metadata.json
baseline-slots.json
baseline-status.json
browser-console.log
page-errors.log
page-install-log.txt
after-install-slots.json
after-install-status.json
after-cleanup-slots.json
screenshot-connect.png
screenshot-install.png
screenshot-final.png
report.json
report.md
```

报告阶段：

| Stage | 含义 |
|---|---|
| E0 | environment |
| E1 | baseline |
| E2 | page |
| E3 | Web Serial connect |
| E4 | slot discovery |
| E5 | play selection |
| E6 | UI install |
| E7 | UI done |
| E8 | device verification |
| E9 | Flash/state verification |
| E10 | cleanup |
| E11 | baseline restoration |

最终必须明确：

```
USB Web UI → Web Serial → ESP32-C3 → Flash : PASS/FAIL
phone-install.js direct path               : NOT USED
mock Web Serial                            : NOT USED
mock /api/install/*                        : NOT USED
DATA                                       : EXISTING browser_smoke COVERAGE
physical power loss                        : NOT TESTED
```

## 11. 与现有测试的边界

| 测试 | 验证层 | 本次 |
|---|---|---|
| `tests/test_phone_install.mjs` | JS 安装协议 / mock fetch | 不改 |
| `tools/realdevice/smoke.py` | 真机 HTTP + Flash 存储契约 | 不改 |
| `tools/realdevice/browser_smoke.mjs` | 真实 phone-install.js × 真机 | 保持 |
| USB Web UI E2E | 页面 × Web Serial × esptool-js × 真机 | 新增 |
| `?mock=1` | USB UI 无硬件走查 | 保持 |

新测试补的是当前缺失的外层链路，不替代已有测试。

## 12. CI / 运行策略

默认 `./tools/validate.sh --static` 不启动真实 USB E2E。

真实 E2E 必须显式 `--real-device)，并要求 self-hosted runner、Chromium、Web Serial、已授权 ESP32-C3、USB 串口、当前 firmware 和 LAN API。

GitHub CI 默认不碰真实硬件。

## 12.1 本地运行

安装浏览器依赖：

```bash
npm install --prefix tools/realdevice
npx --prefix tools/realdevice playwright install chromium
```

首次授权真实串口（只做一次）：

```bash
node tools/realdevice/usb_web_e2e.mjs --authorize --port 4191
```

按提示在打开的 Chromium 页面点击 **Connect** 并选择目标 ESP32-C3。授权完成后关闭浏览器。

正式真机 E2E：

```bash
node tools/realdevice/usb_web_e2e.mjs --real-device \\
  --ip <device-ip> --token <32hex> --play 1 --port 4191
```

`--real-device` 是硬件触碰门禁；不带该参数时 runner 不执行安装。E2E 使用固定独立 profile，授权必须与正式运行使用同一个 profile。

## 13. 实施顺序

1. 提交本设计文档；
2. 给 `install-slot.html` 增加最小 `?e2e=real` port-selection seam；
3. 增加薄浏览器 E2E runner；
4. 保持 mock / Node / real-device 原有测试；
5. 增加静态检查；
6. 无硬件环境执行 host/static 回归；
7. ESP32-C3 真机执行 E0–E11；
8. 修复真机问题；
9. 生成 JSON/Markdown 证据；
10. 最终报告区分 software/static、USB UI real-device 和未测试项。

## 14. 设计审查结论

本设计通过审查：

1. 不重复已有 DATA / phone-install.js 真机测试；
2. 安装动作真正由 UI 触发；
3. Web Serial / Transport / ESPLoader 均为真实实现；
4. UI 与独立 device API 双重验收；
5. baseline + identity cleanup + restoration verification 保证状态中立；
6. real-device 显式 opt-in，默认不碰硬件；
7. 页面生产安装算法不变，测试接入面极小；
8. 故障定位按连接、下载、写入、设备状态分层；
9. 不把软复位冒充物理断电；
10. 最终证明目标明确：

> **USB Web UI → Web Serial → esptool-js → ESP32-C3 → Flash**
