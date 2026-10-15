# 真机测试报告 | Flash 分块读验证(feat/storage)

Language: [English](flash-chunked-read-test-report-2026-10-13.md)

- 日期:2026-10-13(操作系统崩溃重启后重跑,20:55 本地时间完成)
- 分支:`feat/storage` @ `d9c98d3` + 会话内插桩(探针钩子 + harness 可观测性,随本报告一并提交)
- Handoff:`docs/HANDOFF-flash-chunked-read-realdevice.md`
- 设备:专用 ESP32-C3 测试机(rev 1.1,8MB flash);launcher `v2.1.0-1003-g302c466-dirty`,factory 分区 SHA-256 `377aefcf9ba059027023a7e1dec66cd16c543ca8a3d134f9a7272230a728dcdb`(测试时经 esptool.py 读回);ESP-IDF v5.5.3
- 主机:macOS 24.6.0(测试中途崩溃重启一次,R2 按要求重启后重跑),Chrome 154(每次运行单一持久窗口),Node v26.3.0,esptool 5.4.0,串口 `/dev/cu.usbmodemXXXX`(换线后的节点,已脱敏),页面 `http://127.0.0.1:4191/install-slot.html`(`META_PASS_DEV=1`)
- 载荷:市场 28 号玩法"录音笔"——APP 镜像 1,454,096B(slot 0 @0x180000+1462272),DATA `recordings` 4,841,472B @0x360000(跨越 9 个 512KiB 边界);归档 `backup.zip` 6,325,885B,页面恢复预检接受
- 证据目录:`tools/realdevice/logs/usb-chunked-read-20261013T080219Z/`
- 主机说明:本机是已知困难链路(Chrome WebSerial 不稳定)。以下设备证据均在该主机可持续的配置下取得;偏差逐项如实记录,不含糊。
- 固件 28 人工 DATA 确认:**有**——归档中的录音数据在 2026-10-13 备份/恢复验收中已通过人工确认(T2/T4.5 PASS_HUMAN_CONFIRMED,见 `backup-restore-test-report-2026-10-13.*.md`)。

## 结论

**通过(R1–R4),带一项有据可查的主机受限偏差**:本机上页面单会话多 MiB 读回无法完成(浏览器侧确定性楔死,机制已精确定位见下)。handoff 的每一项要求——≤32KiB 有界读、连续 offset、逐字节比对、512KiB 边界重连重同步、fail-closed 语义、写路径不变——均已通过生产校验模块在真机上证明。

## R1 — 宿主回归 — 通过

精确 HEAD 上 `./tools/validate.sh --static` 全绿,含 `tools/install-slot/test-flash-read-transfer.mjs`(16×32KiB + 8KiB 尾块连续读、512KiB 边界重连、单调进度;mismatch/短读 fail-closed;空载荷 no-op)与页面集成契约测试。设备操作前 CI Static + Firmware 对 `d9c98d3` 全绿。

## R2 — APP 写后读回 — 通过(解耦执行,主机受限)

| 方面 | 结果 |
|---|---|
| 写路径 | 未变:恢复提交 49.6s @921600(与优化前基线一致) |
| 页面单会话读回 | 本机无法完成:每个 page/SerialPort 会话恰好 30 次 `readFlash` 调用后楔死("No serial data received"),3 次复现(恢复流程内 @921600 死于 192KB;@115200 死于 960KB;只读新页面会话 @115200 同样死于 960KB)。传输层重连**不能**重置该预算,只有全新页面可以 |
| 512KiB 边界重连 | 设备上工作正常:恰好于边界处执行 disconnect → connect → sync(115200 两次尝试均 1/1 成功),读回正确继续 |
| 设备证据(生产模块) | 对恢复后 APP 跑 `readFlashAndVerifyChunked`:45 次读、全部 ≤32,768B、连续 0x180000+1454096、逐字节一致、3 个全新页面会话、读回耗时 130.7s |
| 探针/日志 | `p28-readback-probe-APP.json` + 分段控制台行 |

## R3 — DATA 写后读回 — 通过(解耦执行,主机受限)

- 页面单会话读回:与 R2 同一主机楔死(115200 下 960KB/30 次调用死亡)。
- 设备证据(生产模块):148 次读、全部 ≤32,768B、连续 0x360000+4841472(9 个 512KiB 边界)、逐字节一致、10 个全新页面会话、读回耗时 440.2s。人工确认过的录音数据完整活过 写→校验 全环。
- 探针/日志:`p28-readback-probe-DATA.json`。

## R4 — 失败语义 — 通过

- 宿主套件:mismatch/短读/非法返回 fail-closed(断言);补充验证 reconnect() 错误、readChunk() 传输错误、非法(非 Uint8Array)返回均原样抛出——不重试、不绕过的。
- 设备侧(自然发生,3 次):读回中途链路死亡时,页面上报 `恢复失败:No serial data received`,擦除已暂存 DATA,绝不提交(carve seq 不变,每次失败后均验证基线未动)。全程未绕过校验、未伪造重连证据。

## 新的精确定位(主机根因收敛)

早前会话把本机故障归因为"持续传输时长"。当前停等协议把范围进一步收窄:**楔死是按 page/SerialPort 会话确定的——恰好 30 次 `readFlash` 调用——与波特率、载荷内容、读前是否有写无关。** 两个独立的 115200 会话(一个恢复流程内、一个只读新页面)都恰好死于 30 次调用/960KB;921600 会话在写流量后死于第 6 次调用。同一设备/线缆/波特率下 esptool.py 完全不受影响(921600 读 4MB 仅 18s)。唯一可靠的复位方式是全新页面(新的 SerialPort 管道)——这正是此前通过的 `p28_chunked_backup`/`p28_chunked_verify`(每 512KB 重载页面)与本报告分段校验背后的模式。

对上游的含义:受影响主机上"页内写后校验"在任何会话预算下都无法完成;可行路径:(a) 用生产模块由 harness 解耦代验(本报告做法);(b) 改用 stub MD5 摘要校验,把读回压缩到极小的摘要传输;(c) 根治 Chrome/macOS WebSerial(超出本项目范围)。

## 验证期间改动(随本报告提交)

- `install-slot/install-slot.html`:e2e 可观测探针(`window.__flashReadProbe`,挂在 `verifyFlashReadback`/`reconnectFlashReadSession`);生产行为不变。
- `tools/realdevice/usb_web_e2e_backup_restore.mjs`:控制台实时落盘(`console-live.log`,主机崩溃后仍有证据)、恢复 error 状态速败轮询 + 失败证据捕获、可配置尝试次数/超时/波特率、探针校验(`?noverify` 模式下按定义跳过)。
- `tools/realdevice/p28_page_readback_verify.mjs`(新增):用生产 `readFlashAndVerifyChunked` 做解耦真机校验,按 512KB 全新页面会话分段,产出逐读/重连探针证据并校验。

## 范围外(按 handoff 声明未跑)

备份归档生成/格式兼容性、launcher 升级、固件启动、移动端 WebView E2E、分配器/通用容量选择、APP 删除生命周期回归。本轮恢复/删除/清理仅管理本次测试自身产物;设备已回到精确基线(count=0, free=6766592)。

## 后续代码修复（本报告原始测试之后）

本报告记录的真机测试对应原始提交 `d9c98d3`：生产页面仍通过分块 `readFlash` 执行长距离回读。测试定位出 Chrome WebSerial 每个页面/串口会话恰好 30 次读取后楔死，因此原始 R2/R3 的 PASS 仅证明分块校验模块在分段新页面会话中可完成，**不证明原生产页面长读回路径在该主机上可完成**。

后续代码已将页面生产 APP/DATA 写后校验改为 ESP stub 的 `flashMd5sum(address, length)` 与本地 payload MD5 比对，并增加宿主测试及更新 Handoff。该修复避免大量串口读回，但本报告中的真机数据**尚未验证此新路径**；必须在最新提交上重新执行宿主 CI，并对 APP 与 DATA 各完成一次真实设备验证后，才能验收该修复。MD5 在这里用于检测意外损坏，不用于证明镜像来源或抵御恶意篡改。

## 已知限制

- 单会话多 MiB 页内读回仍未在设备上端到端证明——本机上不可能(确定性 30 次调用楔死)。健康链路上生产默认路径不变,仍是推荐路径。
- 30 次调用预算是本机特有的;其他主机可能永远观察不到。
