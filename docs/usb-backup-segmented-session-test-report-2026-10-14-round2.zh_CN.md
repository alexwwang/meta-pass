# USB 分段会话备份 —— 第二轮真机测试报告(2026-10-14)

语言:[English](usb-backup-segmented-session-test-report-2026-10-14-round2.md)

## 结论

**PASS。** 成功路径单击一次完成,分块耗时平稳;失败路径在会话重建后从失败偏移重读同一分块,重试上限恰为 3 次,耗尽后明确报错且不产出 ZIP。附一条环境性注记(见下)。

## 被测变动

HEAD `18ed1ae`(v2.1.0-1131)。较第一轮(`db39bf8`)删除了分层读恢复:恢复收敛为单一机制 —— 32 KiB 块失败/超时后重建串口/loader 会话并从同一偏移重读,最多 3 次(每块硬超时 15s);耗尽即中止备份(`b2c76f7`、`624273d`)。

## 结果

### 成功路径(第 3 次尝试)

- 生产 `#btn-backup` 单击一次,单一持久页面会话。**PASS,579.8s**;归档 6,325,913 字节,SHA-256 `f520cb52f4f536d0…`。
- 分块耗时:194 次成功 32 KiB 读 —— min 1786ms、max 3050ms、avg 2844ms。分区域:APP 固件 ≈128s(45 块)、NVS ≈2s、DATA ≈421s(148 块)、11 次会话重建 ≈29s;合计 ≈ 实测 580s。
- 会话:12 次 ESPLoader 同步 = 1 初始 + 11 次计划内 512KiB 段重建(APP 固件 2 次 + DATA 9 次,与段数精确吻合)。每次重建均执行 carve 元数据守卫。
- `manifest.complete=true`;载荷页内 3/3 验证;ZIP 哈希与页内计算值一致。

### 失败路径(第 1、2 次,本主机真实停滞)

| 尝试 | 耗时 | 读错误 | 失败驱动重建 | 终态分块 | 最终报错 | ZIP |
|---|---|---|---|---|---|---|
| 1 | 227s | 6 | 5 | `0x3a8000` attempt 3/3 | `备份失败:No serial data received.` | 无 |
| 2 | 190s | 4 | 3 | `0x398000` attempt 3/3 | `备份失败:No serial data received.` | 无 |

- 每个失败块均在会话重建后**从同一 flash 偏移**重读 —— 生产路径证实失败偏移续读。
- 每块重试上限恰为 **3 次**(观察到 `1/3`、`2/3`、`3/3` 全部标记),符合 `BACKUP_RETRIES=3`。
- 终态失败明确(error 样式 + 清晰文案),**不提供不完整 ZIP**。

### 独立验证

- esptool.py 只读 dump 与归档逐字节一致:APP 固件 `f519f076…`、槽位尾块 `f78752cb…`、DATA `cba83ea5…`;槽内填充全 0xFF 符合设计(`t6-byte-compare.json`,PASS)。
- 正常 DTR/RTS 复位重启:LAN listing 与备份前逐字节一致,组元数据不变。

## 环境注记

首次停滞后,第 1、2 次尝试几乎每块都停滞,即使在新会话中(Chrome WebSerial 主机传输死亡)。第 2 次后立刻 esptool 探活:512KiB @ 808kbit/s 零错误 —— 设备与链路健康。第 3 次同代码同设备连续 580s 零停滞。简化恢复能扛孤立停滞(第一轮已实证),无法补偿持续退化的主机浏览器传输;此时按规格干净失败。

## 门禁

只读:未删除、未恢复。Play 28 组保留在设备上。

## 证据(本地,gitignored)

`tools/realdevice/logs/20261014T052317Z-usb-backup-segmented-round2/` —— `report.md`/`report.zh_CN.md`/`report.json`、`environment.txt`、`pre-backup-slots.json`、`failure-retry-analysis.json`、`t7-post-reboot-slots.json`,及每次尝试的 rundir(`backup-rundir`、`backup-rundir-attempt1-fail`、`backup-rundir-attempt2-fail`),各含驱动日志、`backup-read-result.json`、console 日志、ZIP/manifest(仅 PASS)、失败状态+截图(失败运行)、dump 与 `t6-byte-compare.json`。

## 环境

设备固件未变:`meta-pass_v2.1.0-1109-g…` factory(sha256 `377aefcf…`)。macOS 15.7.4、Chrome 155.0.8059.40、Node v26.3.0、esptool 5.4.0、ESP-IDF 5.5.3。token/IP/串口已脱敏。
