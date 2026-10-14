# USB 分段会话备份读取 —— 真机测试报告(2026-10-14)

语言:[English](usb-backup-segmented-session-test-report-2026-10-14.md)

## 结论

**PASS。** 在此前单会话读取必死的主机上,生产 USB 页面备份按钮(`#btn-backup`)现在单页面会话即可完成完整 APP+DATA+NVS 归档。单击一次,无重载,无宿主侧拼装。

## 被测变动

`feat/storage` 分支 commit `db39bf8`,核心修复 `ee64d79 fix(backup): renew serial session between bounded flash read segments`:页面将备份读取拆为 512 KiB 段,段间重建 Transport/ESPLoader;会话重建失败即中止,绝不产出不完整 ZIP。

## 结果

| 通过标准(handoff) | 结果 |
|---|---|
| 生产 UI 单击一次备份;无需手动重载页面或宿主分段脚本 | PASS —— 驱动单击 `#btn-backup` 一次,单一持久页面会话 |
| 归档完整、仅含一个 APP 组及其关联 DATA | PASS —— `manifest.complete=true`,manifest_version 2,一槽 + 一 DATA 记录 + NVS |
| 各载荷字节数与 SHA-256 准确 | PASS —— 页内 3/3 验证;ZIP SHA-256 `3b578dd15f85…`(与页内计算值一致) |
| 独立 esptool.py dump 与归档 APP/DATA 载荷逐字节一致 | PASS —— APP 固件 `f519f076…`、槽位尾块 `f78752cb…`、DATA `cba83ea5…` 均逐字节一致(尾块 = 槽末尾 4 KiB 的 `MNAM` 名称记录;槽内 4 KiB 0xFF 填充按设计不入归档) |
| 备份前后及重启后 APP/DATA 所有权记录不变 | PASS —— 三个检查点 LAN listing 完全一致(正常 DTR/RTS 复位,非断电) |
| 任一分段/必要读取失败时不得把 ZIP 呈现为成功归档 | PASS(设计演示)—— 已知主机传输停滞在 `0x3d8000` 真实出现一次,页内会话重建成功恢复;未观察到部分 ZIP 路径 |

备份:577.9s @115200,归档 6,325,913 字节。逐读节奏平稳(~2.85s/32KiB)。

## 会话重建在生产条件下真实触发

读到 `0x3d8000`(DATA,约 22 分钟处)时撞上 `Packet content transfer stopped` —— 正是本主机此前四次单会话尝试的全部死因(见[上一轮报告 Stage C](usb-data-backup-read-test-report-2026-10-14.md))。页面打印 `DATA backup: opening a fresh serial/loader session at segment boundary`,重建 loader 并校验 carve 元数据未变后继续,直至归档完整。

## 事故

1. **场景搭建的恢复阶段在 921600 两次失败**:波特率切换后首个 4KiB carve 记录读超时(两扇区、两次连接皆然),页面回退 legacy 模式并(正确地)拒绝事务恢复。改用 harness 支持的 `state.baud=115200` 后 55.4s 成功。属连接期首读抖动,与备份中途停滞不同。备份本身全程 115200。
2. `usb_data_backup_read_once.mjs` 不自带 dev server;须以 `META_PASS_DEV=1` 启动(否则 `mock-device.js` 404 卡 `__installSlotReady`)。成功运行前有两次失败尝试,证据已保留。

## 门禁

遵守只读门禁:未删除、未做备份后恢复。Play 28 组保留在设备上。

## 证据(本地,gitignored)

`tools/realdevice/logs/20261014T032316Z-usb-backup-segmented-session/` —— `report.md`/`report.zh_CN.md`/`report.json`、`environment.txt`、`install-rundir/`(建组)、`backup-rundir/`(驱动结果 JSON 含逐读统计、ZIP、manifest、页面日志、含会话重建 trace 的 console、`t6-byte-compare.json`、esptool dump 日志、`t7-post-reboot-slots.json`)。

## 环境

设备固件未变:`meta-pass_v2.1.0-1109-g…` factory(sha256 `377aefcf…`)。主机:macOS darwin 24.6.0、Chrome 155.0.8059.40、Node v26.3.0、esptool 5.4.0、ESP-IDF 5.5.3。token/IP/串口已脱敏。
