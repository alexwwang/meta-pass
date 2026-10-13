[English](HANDOFF-flash-chunked-read-realdevice.md)

# Handoff｜feat/storage Flash 分块读取真机验证

日期：2026-10-13  
目标分支：`feat/storage`  
适用提交：交接执行时的最新 `origin/feat/storage` HEAD；不得沿用旧提交的 PASS。

## 1. 目标

验证 Flash 长距离读取是否能通过有限大小的分块及周期性串口会话重建，避免 Chrome Web Serial / esptool-js 持续读取数分钟后挂起。此次修复目标是**读取和读回校验**，不是分块写入优化。

当前实现预期：
- APP 和 DATA 写入后的生产读回校验使用 ESP stub 的 `flashMd5sum(address, length)`，设备端对完整目标范围计算 MD5，浏览器对待写入 payload 计算 MD5 并严格比较。
- 每个非空 APP/DATA 范围只需一个设备端摘要请求，不再把整个范围经 Web Serial 分块读回，规避报告中复现的单 page/SerialPort 会话 30 次 `readFlash` 调用后楔死问题。
- 设备摘要格式异常、摘要不匹配、MD5 实现不可用或串口命令失败均必须失败关闭，不得提交恢复事务。
- `flash-read-transfer.js` 保留为独立分块/逐字节校验工具，用于补充真机取证；它不再是页面生产写后校验的执行路径。
- 原有 Flash 擦除与写入流程保持不变。

## 2. 开始前的门禁

在本地仓库执行并记录真实输出：

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
git status --short
git rev-parse HEAD
./tools/validate.sh
```

确认当前 HEAD 的 Static checks 和 Firmware checks 均通过，并记录固件 SHA-256、设备标识（可脱敏）、浏览器/OS、ESP-IDF、Node 版本、串口端口及页面入口。若工作区不干净、commit 不匹配、构建或宿主测试失败，先停止并报告，不要开始破坏性真机操作。

## 3. 影响面评估与最小回归集

本次运行时代码改动集中在 USB 服务页面的**写入后 Flash 回读校验**：
- 新增生产校验模块：`install-slot/flash-readback-verify.js`，使用设备端 Flash MD5 与本地 payload MD5 比对完整范围。
- 页面接入：`install-slot/install-slot.html` 的 APP 回读和 DATA 写入/恢复后的回读都使用摘要校验。
- `install-slot/flash-read-transfer.js` 与其测试仍保留，用于独立分块/逐字节真机验证，不代表页面生产路径继续执行长读回。
- 服务与自动化检查：`tools/install-slot/server.mjs`、`tools/install-slot/test-flash-readback-verify.mjs` 及页面契约测试。

本次优化没有修改擦除/写入循环、分区分配器、设备固件/bootloader、手机内嵌页面、安装/删除规划或归档格式。因此，不要仅为这次改动重跑无关的手机页面、分配器、启动、完整安装/删除生命周期或通用备份格式测试。

只执行以下回归项：

### R1 — 宿主自动化回归（必需，不写设备）
在准确的 HEAD 上执行 `./tools/validate.sh --static`。确认运行 `test-flash-readback-verify.mjs`、`test-flash-read-transfer.mjs` 和页面接线契约测试。摘要校验测试必须覆盖完整范围只发起一次设备摘要请求、摘要匹配、摘要不匹配、非法摘要、MD5 实现异常、串口命令失败和空 payload 拒绝；分块工具仍覆盖偏移/块大小/重连/短读/字节不匹配。Firmware checks 是使用该页面/固件进行真机测试的前置条件。

### R2 — APP 写入后回读（必需，真机）
使用 USB 服务页面安装或恢复获准测试 APP（大于 512 KiB，最好数 MiB）。确认原有写入正常完成，随后生产路径执行设备端 `flashMd5sum` 校验；日志应记录目标地址、长度、期望 MD5、设备 MD5 和完成状态，且两个摘要一致。该路径不应触发整段 `readFlash` 流。

### R3 — DATA 写入/恢复后回读（必需，真机）
使用现有且安全的测试 APP/DATA 环境，执行 DATA 恢复/写入路径，并使用超过 512 KiB（最好数 MiB）的 payload。确认写入行为没有变化，设备端对完整 DATA 范围计算 MD5，期望/实际摘要一致，且事务成功提交。不要为了扩大覆盖面而额外创建或删除无关槽位。

### R4 — 重连失败语义（宿主测试必需；真机仅在自然遇到失败时取证）
宿主测试必须证明摘要不匹配、非法摘要、MD5 实现异常、设备摘要命令/串口失败均失败关闭。真机若摘要校验失败或串口异常，保留日志并确认恢复事务未提交、暂存 DATA 已按现有失败路径清理；不得绕过校验后标记通过。

### 明确不在本次回归范围内

本次仅改回读校验，不要求重跑完整备份归档创建/格式兼容、launcher 升级、固件启动、手机 WebView/浏览器 E2E、通用 DATA 分区大小选择、分配器或 APP 删除生命周期测试。现有备份创建、probe 和元数据读取没有接入新 helper。如果无法安全执行 DATA 恢复/写入测试，应将 R3 标记为 BLOCKED，不要用无关测试替代。

## 4. 通过标准

- R1–R4 中要求的宿主检查通过；被测 HEAD 的最新 Static checks 和 Firmware checks 均为绿色。
- APP 和 DATA 真机写后校验均通过设备端完整范围 MD5，日志中的期望/实际摘要一致。
- 不再要求生产路径出现 32 KiB 分块读取或 512 KiB 重连日志；生产路径应避免长距离 Web Serial 读回。
- 无摘要不匹配、非法响应、未捕获的命令错误、无法解释的超时或永久挂起；失败时事务 fail-closed。
- 原有擦除/写入行为未改变；不得声称已回归未测试的备份、手机页面或生命周期路径。
- 报告记录准确 HEAD、固件 SHA-256、payload 大小、块数、重连次数/耗时、逐字节校验结果、日志路径，以及每项 PASS/FAIL/BLOCKED。

## 5. 失败取证与归因

失败时立即停止后续破坏性操作，保留：
- 完整页面日志、浏览器控制台错误和可获得的 UART 日志；
- 读取地址、请求长度、实际返回长度、块序号、累计字节数；
- disconnect/connect/sync 的时间戳、耗时、异常与重试情况；
- 设备 APP/DATA 清单、当前 commit、固件 SHA-256；
- 自动化报告与实际执行命令/退出码。

初步归因：
- Host test 失败：分块算法或测试问题。
- 块偏移不连续、短读或字节不一致：读取协议、缓冲区或地址计算问题。
- 重新连接/同步失败：transport 生命周期、stub/loader 会话或串口恢复问题。
- 仅长时间读取挂起而短读取通过：仍未解决的持续读取/会话稳定性问题。
- 设备重启、panic 或分区范围异常：固件/设备问题；保留现场并停止。

不要通过放宽校验、跳过读回或伪造重连日志来通过测试。

## 6. 交付报告

报告至少包含：
- 测试时间、分支与完整 commit SHA；
- 固件 SHA-256、设备/固件类型、OS/浏览器/Node 版本；
- APP/DATA 的字节数、块数、512 KiB 边界数和重连次数；
- 每阶段耗时、错误/重试、逐字节校验及 hash 结果；
- 页面/UART/浏览器日志与报告路径；
- 结论：PASS / FAIL / BLOCKED，以及明确的失败归因；
- 是否完成真实市场 28 号玩法的人工 DATA 确认。未确认不得标为 PASS。

CI 通过只是进入真机测试的前置条件，不代表本 handoff 的真机验收已通过。
