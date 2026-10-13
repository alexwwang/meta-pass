# 真机备份/恢复测试报告(USB 页,feat/storage)

Language: [English](backup-restore-test-report-2026-10-13.md)

- 日期:2026-10-13
- 分支:`feat/storage` @ `6042678` + 会话内修复(见"发现与修复";修复后 CI 重新验证)
- 设备:ESP32-C3 专用测试机(标识已脱敏),launcher 固件为本分支 test-control 构建
- 页面入口:USB 服务页(`install-slot.html`)
- Handoff:`docs/HANDOFF-realdevice-backup-restore-manual-data-confirmation.zh_CN.md`
- 证据:
  - data-child 轮:`tools/realdevice/logs/usb-backup-restore-20261013T003913Z/`
  - **28 号玩法轮(完整生命周期,人工门禁):`tools/realdevice/logs/usb-backup-restore-p28-20261013T012352Z/`**

## 结论

**通过(28 号玩法,真实 UI 固件,人工门禁)。** 完整生命周期在真机上以字节级证据闭环:安装 → 人工 DATA 确认 → 备份 → 删除(APP + 关联 DATA)→ v2 恢复 → 字节级保真校验 → 人工确认恢复后数据 → 清理回基线。此前 data-child 轮作为自动化层证据仍然有效;其 T2 人工门禁被测试人否决(无头 fixture 无 UI),正是它促成了 28 号玩法轮。

## 28 号玩法场景记录(handoff §6 模板,精简)

### T0 — 基线

count=0,free=6766592,installer 空闲。

### T1 — 安装 APP+DATA(市场 28 号玩法"录音笔",USB 页 community 页签)

| 字段 | 内容 |
|---|---|
| APP/play_id | `REC-P28-HUMAN-GATE` / 2147483648 |
| 槽位/DATA | slot 0 @0x180000+1462272;DATA 记录 `recordings` subtype 0x81 @0x360000+4841472(约 4.6MB) |
| 结果 | **通过**:槽位 + DATA 跨 USB 硬复位存活;free 6766592→1441792 |

### T2 — 人工 DATA 确认 —— **PASS_HUMAN_CONFIRMED**

测试人在设备上验证:录音笔能启动、能录音、重听/重启后录音保持。(此前 `data-child` fixture 在同一门禁被否决——无头串口测试固件无 UI;"死机"归因于 fixture 无内容可渲染,属设计使然。)

### T3 — 备份

| 字段 | 内容 |
|---|---|
| 页面一体备份 | **3 次全败**——任何波特率下 Web Serial 链路持续传输 ~4-5 分钟即死(slot+NVS+~2.6MB DATA 后 8/8 重试全败,跨重连复现) |
| esptool.py 对照实验 | 921600 读 4MB 仅 18s、零失败(约 233KB/s)→ **设备/线缆/USB 路径完全健康;持续传输死亡在 Chrome WebSerial ↔ esptool-js 路径** |
| 采用的降级 | host 侧**分块备份**(每 512KB 一个全新 Web Serial 会话 @115200,本链路唯一存活的读模式):APP 镜像 1,454,096B + tail 4,096B + DATA 4,841,472B(10 块)+ NVS 24,576B;归档用与页面相同的共享模块拼装 |
| 归档 | `backup.zip` 6,325,885B;manifest v2,`complete: true`,恰好一个 APP+DATA 组 + `nvs.bin` 完整性条目 |
| 校验 | 每个条目长度 + SHA-256 经 `readDataArchiveZip`/`validateDataArchiveManifest`/`validateDataArchivePayloads` 验证;**页面自身的恢复预检接受了该归档**(`#restore-info.ok`) |
| NVS 卫生 | NVS 原始字节从不写入证据/日志(仅哈希) |

### T5a — 删除(仅在备份校验通过 + T2 通过后)

**通过**:页面 UI 删除 APP;关联 `recordings` DATA 随之删除(carve 提交 seq 53→54);复位后 count=0、data=0、free=6766592。

### T4 — 从归档 v2 恢复

| 字段 | 内容 |
|---|---|
| 页内读回恢复 | 在本机不可行:读回是持续流式大读(会死);115200 下恢复总时长实测 >40 分钟 |
| 采用的降级 | 页面 `?noverify` 选项(跳过页内逐字节读回;写与 carve 提交不变)@921600——**恢复 50.7s 提交完成** |
| 恢复后状态 | slot 0 @0x180000+1462272,`recordings` @0x360000+4841472,名称保留,跨复位存活 |
| 字节保真(harness 分块校验,512KB/新会话) | **恢复后 APP == 归档 APP**(sha256 `f519f076…`);**恢复后 DATA == 归档 DATA**(sha256 `cba83ea5…`)——测试人的录音**逐字节**活过了删除+恢复 |
| NVS | 恢复从不写 NVS(代码路径 + 宿主测试);真机旁证:存于 NVS 的配对 token 在恢复后持续有效(恢复后全部 API 调用成功),且 NVS 在空闲重启间字节稳定——before/after 哈希差异归因于期间的测试生命周期操作(删除会写持久 remove-intent 键),而非恢复窗口 |

### T4.5 — 恢复后人工确认 —— **PASS_HUMAN_CONFIRMED**

测试人在设备上验证:恢复后的录音笔能启动,删除前的录音可播放。

### T5b — 清理

**通过**:恢复的 APP 经页面 UI 删除;基线精确恢复(count=0,free=6766592)。

## data-child 轮(自动化层,本轮前半段)

- T0/T1/T3 自动化证据保持有效,存于 `usb-backup-restore-20261013T003913Z/`:安装 + runtime 串口 DATA IO(写/读/校验和/重启/重读)+ 备份 zip 与活体 DATA 字节一致,全部通过。
- T2 人工门禁:该 fixture **被否决**(无 UI;"死机"=无内容可渲染,设计使然)。该轮 T4/T5 未执行。

## 发现与修复(均已入库)

1. `install-slot/install-slot.html`:
   - `?baud=115200` 选项:会话固定在 ROM 波特率。
   - **读回批次化**:恢复/安装读回从每调用 4KB 改为 64KB(命令往返次数降 16×;MB 级读回不再被每调用开销主导)。
   - `?noverify` 选项:跳过页内读回(仅 e2e/诊断;生产默认不变)。必要原因:本机 Chrome WebSerial 无法承受 esptool-js 持续流式读,而同线缆同设备的 esptool.py 毫无问题。
2. `tools/install-slot/server.mjs`:dev server 白名单补 `data-archive-restore.js`(上游新模块;旧 dev server 进程导致页面 404)。
3. `tools/realdevice/mobile_page_runtime_driver.py`:`launcher_hello` 冲洗 E2E stdin 通道(esptool-js SLIP 残留可卡死 launcher 的阻塞式丢弃循环)。
4. 新增真机工具:`usb_web_e2e_backup_restore.mjs`(T0–T5 驱动,单一 Chrome 窗口,人工门禁文件等待,串口死亡自愈)、`install_market_play.mjs`、`p28_chunked_backup.mjs`、`p28_chunked_verify.mjs`、`restore_watch.mjs`。
5. 固件观察(仅测试代码,未修):`meta_e2e_control.c` 用阻塞 `fgets(stdin)` + 阻塞等换行丢弃——无 `\n` 的二进制垃圾会将其楔死直到外部换行。建议后续改为有界/非阻塞行拼装。
6. 环境根因(为后续运行存档):本机 Mac+Chrome+WebSerial 路径上,esptool-js 持续传输数分钟必死(任何波特率);esptool.py 不受影响。批量读应走"分块新会话"或 esptool.py;页面页内读回需要改为 stub-MD5 校验(线上只传摘要)才能在本机可靠。

## 已知限制

- 页面页内读回校验未能在本机端到端执行(WebSerial 持续读限制);等价性由 harness 分块校验相同字节保证。
- 恢复以 `?noverify` 执行;生产默认(读回开)不变,在健康链路上仍是推荐路径。
