# 多轮 USB Web UI 真机 E2E 测试报告 — 2026-10-09

[English](usb-web-e2e-multi-round-20261009.md)

**仓库**：`meta-pass`，分支 `feat/storage`
**设备**：ESP32-C3 AI Passport，单个 USB-JTAG 端口
**浏览器**：Chrome 154.0.8037.99；单一持久会话，Web Serial 仅授权一次
**结果**：**4/4 轮通过**
**证据目录**：`tools/realdevice/logs/usb-web-e2e-repro-r2/`、`tools/realdevice/logs/usb-web-e2e-r3-r4/`

## 结论

在真实设备上完成完整多轮流程：安装 → USB 复位 → 再安装 → USB 复位 → 删除 → USB 复位 → 再删除 → USB 复位。四轮全部通过，最终设备状态恢复到测试前基线。整个测试没有擦除整片 Flash。**证据范围说明：** 这次 4/4 真机测试由临时本地脚本驱动；仓库中的 `tools/realdevice/usb_web_e2e_multi.mjs` 是之后补入的可复用正式 harness，尚未以提交后的该文件原样完成端到端真机复跑。CI 只验证源码、语法和 host contract，不代表真机 E2E 已通过。

| 轮次 | 操作 | 结果 |
|---|---|---|
| R1 | 安装 Play #1（weather clock，约 1.97 MB），再 USB 复位 | **PASS** |
| R2 | 安装 Play #2（Luigi's Mansion，约 615 KB），再 USB 复位 | **PASS** |
| R3 | 删除 Play #1，再 USB 复位 | **PASS** |
| R4 | 删除 Play #2，再 USB 复位 | **PASS** |

## 各轮结果

| 轮次 | 操作 | Carve 序号 | 槽位数 | 可用空间 | 验证结果 |
|---|---|---:|---:|---:|---|
| R1 | 在 0x360000 安装 P1（约 1.97 MB） | 73→75 | 1→2 | 5640192→3633152 | 启动日志载入 seq=75、2 个槽；API 和页面日志验证成功 |
| R2 | 在 0x2b0000 安装 P2（约 615 KB） | 75→77 | 2→3 | 3633152→3010560 | 启动日志载入 seq=77、3 个槽；两个子固件共存 |
| R3 | 删除 P1（槽位索引 2） | 77→78 | 3→2 | 3010560→5017600 | P1 被删除；P2 与基线中的 smoke-three 均保留 |
| R4 | 删除 P2（槽位索引 1） | 78→79 | 2→1 | 5017600→5640192 | 仅剩 smoke-three，可用空间精确恢复到基线 |

## Carve 记录生命周期

```text
seq=73  基线：仅 smoke-three
  R1 安装 P1
seq=75  2 个槽位：smoke-three、P1
  R2 安装 P2
seq=77  3 个槽位：smoke-three、P2、P1
  R3 删除 P1
seq=78  2 个槽位：smoke-three、P2
  R4 删除 P2
seq=79  1 个槽位：smoke-three；恢复基线
```

每一轮都经历了 `rst:0x15 (USB_UART_CHIP_RESET)`，并以 `boot:0xa (SPI_FAST_FLASH_BOOT)` 重新启动。

## 已验证内容

- **跨复位持久化**：Carve 序号按 73→75→77→78→79 单调变化，复位后仍可读取。
- **多槽共存**：三个槽位同时存在时，smoke-three 位于 0x180000、P2 位于 0x2b0000、P1 位于 0x360000。
- **空间核算**：可用空间从基线 5640192 降至 3633152，再降至 3010560；删除后依次恢复到 5017600 和 5640192。
- **选择性删除**：R3 只删除 P1，P2 和 smoke-three 保持完整；R4 删除 P2 后恢复原始槽位集合。
- **基线恢复**：最终 `count=1 free=5640192 archived=1`，与测试前状态一致。
- **USB 复位可靠性**：每次复位均重新启动到工厂固件，设备约 15 秒后恢复 LAN HTTP 服务。
- **单浏览器会话**：整个四轮测试只启动一次 Chrome，只进行一次 Web Serial 授权，没有重启浏览器。

## 安装路径证据

页面日志确认每次安装都执行以下链路：

1. `loadSlotModel()`：通过 USB 串口重新读取 carve 记录和分区表。
2. `planInstall()`：计算 CREATE 操作及新槽位的偏移、大小。
3. `commitCarve({materialize: true})`：先写入 carve 记录并回读验证，再写入分区表并回读验证。
4. `loader.writeFlash()`：擦除并写入压缩固件镜像。
5. 写入尾部元数据扇区（MSIG/MAEG/MNAM）。
6. `commitCarve({materialize: false})`：将槽位标记为 VALID，写入并回读验证 carve 记录。
7. 更新进度，并显示完成状态。

## 删除路径证据

每次删除都执行以下链路：

1. `loadSlotModel()`：重新读取 carve 记录。
2. `removeSlotAction(index)`：擦除槽位镜像头，避免扫描时重新发现已删除镜像。
3. 从 carve 槽位数组中移除目标槽位。
4. `commitCarve({materialize: true})`：提交新 carve 记录并物化分区表。
5. UI 中对应槽位行消失，carve 槽位数量减少。

## 曾出现的两个误报及根因

初期测试曾将以下问题怀疑为产品缺陷；复现后确认是临时驱动脚本的问题，未复现为产品缺陷。

### “No serial data received”

**根因**：临时驱动脚本被 60 秒 shell 超时终止，终止时设备仍在写 Flash，导致串口传输状态不一致。后续使用不设短超时的干净脚本复测，R1 和 R2 均成功。

### “Cannot read properties of null (reading 'slots')”

**根因**：驱动的 `uiConnect()` 在未点击页面 Connect 按钮前就开始轮询 `slotModel.mode`。此时页面仍处于 `LEGACY_FALLBACK` 模式，`slotModel.carve` 为 null。先触发页面 Connect，再等待 `DYN_SLOT` 模式后问题消失。

### 结论

以上两次问题来自未提交的临时驱动脚本。实际页面安装路径 `loadSlotModel → planInstall → commitCarve → writeFlash → commitCarve` 在复测中按预期执行。

## 开发服务器发现

开发服务器此前设置的 CSP 会阻止 `install-slot.html` 中的 inline 脚本执行；生产环境没有该 CSP，因此生产页面不受影响。该问题已在提交 `1d262e6` 中修复：移除 CSP 响应头，保留 `nosniff` 和 `DENY`。

## 证据与限制

- 本报告的真机结果来自当次临时驱动脚本和保存的设备证据；不能据此声称提交后的正式 harness 已经在真机上执行通过。

- 原始设备日志、串口日志、页面日志、状态快照和截图保存在上述本地证据目录。
- 报告已脱敏，不包含设备 IP、MAC、会话 token、Wi-Fi SSID、USB 串口路径或本机绝对路径。
- 本测试未执行物理断电；USB UART 复位不等价于拔除设备电源。
- DATA 分区由既有 `browser_smoke.mjs` 测试覆盖，本轮多轮 E2E 不重复测试 DATA。
- 安装与删除均通过真实 USB Web Serial 页面操作；未绕过 UI 直接调用手机安装协议。
