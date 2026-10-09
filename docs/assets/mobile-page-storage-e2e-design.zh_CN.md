# 手机内嵌安装页：空间管理与子固件安装/卸载 E2E

<p align="right">
  <a href="mobile-page-storage-e2e-design.md">English</a> · <strong>简体中文</strong>
</p>

日期：2026-10-09  
分支：`feat/storage`

## 目标

使用手机视口浏览器直接操作设备提供的安装页面；通过设备只读 API 和设备侧串口运行证据独立校验结果。不要以模拟器替代浏览器视口测试，也不要把仅有 UI 成功提示当成设备端安装成功。

当前主执行器：`tools/realdevice/mobile_page_e2e.mjs`  
设备侧驱动接口：`tools/realdevice/mobile_page_runtime_driver.py`  
子固件串口协议客户端：`tools/realdevice/data_child_serial.py`

## 已实现的真实设备测试路径

| 阶段 | 自动化动作 | 必须满足的后置条件 |
|---|---|---|
| M01 页面与视口 | 打开真实设备页面；检查内容、交互控件、触控能力；测试 320×720、360×800、390×844、430×932 | 页面可用、关键控件可见、无横向溢出、无页面/控制台/请求错误 |
| M02 基线与安全门 | 读取 slots、DATA reservations、installer status | 安装器空闲；分区池范围、对齐、大小粒度、重叠和 free 字节计算一致；否则拒绝写设备 |
| M04 安装 A | 从市场按 play ID 查找、选择槽位并安装；独立读取设备状态 | UI 显示完成且设备 API 出现 VALID 槽；空间几何正确；原有 DATA reservation 不丢失 |
| M04A 子固件 A | 运行设备侧 runtime driver | 子固件确实启动；DATA 擦除、写入、读回、校验和、重启持久性全部有串口证据 |
| M05 安装 B | 安装第二个 play ID | A/B 可同时存在；无重叠；原有 DATA reservation 保留 |
| M05C A/B 隔离 | 比较设备侧报告的 DATA 物理地址 | A/B DATA extent 不同，不能仅凭 play ID 不同就判定隔离成立 |
| M06 卸载 A | 在管理 UI 中双击删除、确认；卸载后运行 B | A 从槽表消失；B 仍 VALID 且可启动；A 的 DATA reservation 被释放；池几何仍正确 |
| M07 卸载 B | 同样从 UI 删除并验证 | B 从槽表消失；设备侧证明已删除镜像不可启动且 DATA 已释放 |
| M08 恢复基线 | 再读 slots、reservations 和 status | 安装器空闲；槽表、DATA reservations 和 free bytes 与测试前完全一致 |

## 数据与空间管理断言

测试必须分别验证以下四个层次，不能互相替代：

1. **UI 层**：管理列表、槽位状态、删除确认、安装进度及完成/失败反馈。
2. **设备 API 层**：`/api/install/slots` 和 `/api/install/status` 的独立读回。
3. **分配器层**：每个 app slot / DATA reservation 都在动态池范围内、按规则对齐、彼此不重叠，且 `free = pool bytes - occupied bytes`。
4. **设备运行层**：测试子固件真实启动并通过 USB Serial/JTAG 提供 DATA 擦除、写入、读回、checksum、重启持久性及返回启动器的证据。

删除后的 UI 行消失不代表 Flash 镜像已不可启动；DATA reservation 从 API 消失也不等于子固件 DATA 内容经过了正确验证。需要 runtime driver 对应证据。

## 安全与可复现性

- 真机写操作必须显式传入 `--real-device`；禁止默认执行破坏性测试。
- 只允许测试运行器创建带随机后缀的专属测试名称，不能卸载基线中已有的用户槽位。
- 测试前若安装器非空闲、槽位几何非法或测试名称冲突，必须停止，不得尝试“自动修复”设备。
- 失败后仅清理本轮创建的测试槽位；最终报告必须记录清理失败。
- 日志不得包含 session token、完整设备 URL、局域网 IP 或主机敏感路径。
- `--manual-assist` 只能报告 `PASS_WITH_MANUAL_STEPS`，不得宣称全自动通过；没有设备侧证据的 DATA 持久性和删除不可启动性必须标记为未覆盖。
- 该 E2E 是显式派发的硬件测试，不应在普通 CI 中自动烧写设备。

## 当前证据边界

主执行器已经包含 M01–M08 的 UI + 设备 API + runtime driver 测试流程，并在 `--require-data-reservation` 模式下检查 DATA reservation、A/B 物理隔离、卸载释放和基线恢复。本次新增静态契约测试，防止后续改动意外删除这些门禁。

这不代表本轮已经连接真机运行了 E2E。真实设备结果必须以 `tools/realdevice/logs/<run>/report.json` 及其脱敏证据为准。市场固件的 DATA 行为分析也不能由测试子固件的成功代替。
