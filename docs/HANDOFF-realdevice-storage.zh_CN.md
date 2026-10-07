[English](HANDOFF-realdevice-storage.md)

# Handoff｜feat/storage 真机验收与后续修复

## 0. 目的

本文件是给后续本地 coding agent 的**可执行 handoff**。读取后，不要只给测试建议；应在本地真实连接 ESP32-C3 Passport 的环境中完成真机自动化测试，生成完整报告、日志和证据；如果发现失败，先归因，再回到 `feat/storage` 修改代码、补测试、重新执行，直到通过或明确证明是环境/硬件阻塞。

当前代码分支：`feat/storage`。
不要在 `main` 上开发。

## 1. 验收边界

本次功能目标是动态安装存储链路，重点验收：

1. APP + DATA manifest。
2. APP/DATA 联合容量计算。
3. dynamic carve / slot placement。
4. ARCHIVED 自动回收。
5. 409 no-fit 结构化返回。
6. carve record-first 原子提交。
7. launcher / active child runtime table 派生。
8. Child DATA initial image。
9. DATA offset resume。
10. DATA done 幂等跳过。
11. APP + DATA finalize 完整性。
12. slot state persistence。
13. materialization 后 partition cache refresh。
14. boot hook + durable carve record 的重启恢复。
15. deep-sleep child DATA isolation。
16. legacy carve record compatibility。
17. CRC + partition-table MD5 validation。
18. bootloader size / rodata budget。

Host TDD 与 firmware build 已有 CI；真机验收不放 GitHub CI。

## 2. 测试环境

必须具备：

- macOS 或 Linux。
- ESP32-C3 AI Passport 真机，8 MB Flash。
- 可传输数据的 USB-C 数据线。
- USB Serial/JTAG 可枚举。
- ESP-IDF 5.5.3。
- Python 3、Node.js >= 20、esptool、pyserial。
- 手机/电脑与 Passport 在同一局域网。
- Passport IP 地址。
- 仓库当前 `feat/storage` 工作区干净。

准备：

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py --version
python3 -m serial.tools.list_ports -v
```

预期 ESP-IDF 为 5.5.3，USB 端口可见。

## 3. simulator 验证

passport-sim 仓库：

https://github.com/VOID001/FoloToy-Passport-Simulator

本环境必须使用真实 simulator，不得用 mock 冒充。

本地准备：

```bash
git clone https://github.com/VOID001/FoloToy-Passport-Simulator.git ../passport-sim
cd ../passport-sim
npm install
cd ../meta-pass
./tools/validate.sh --firmware
PASSPORT_SIM_DIR=../passport-sim ./tools/validate.sh --sim
```

成功标准：

- QEMU ESP32-C3 真正加载固件。
- bootloader 正常启动。
- partition table 无错误。
- meta boot hook 不 panic。
- application 启动并产生显示帧。
- 已有 sim test 全部 PASS。

注意：当前执行环境不能访问 GitHub 网络，因此本轮只能确认 simulator 仓库结构和代码入口，不能声称 simulator 已实际执行通过。后续有本地网络环境时必须实际执行。

## 4. 真机日志采集

不要依赖人工复制 terminal。

smoke 必须独占 USB 串口，分阶段：

- monitor 运行时负责持续收集 UART。
- esptool 操作前停止 monitor。
- esptool 完成后重新启动 monitor。
- 所有 UART 输出保存到 run artifact 目录。
- esptool stdout/stderr 也保存。
- 失败时保存 UART tail、当前 slots/status、关键 Flash dump。

手工诊断命令：

```bash
idf.py -p /dev/cu.usbmodemXXXX monitor --timestamps
```

Linux 示例：

```bash
idf.py -p /dev/ttyACM0 monitor --timestamps
```

不要同时让两个进程占 USB serial。

## 5. 真机自动化入口

主入口：

```bash
python3 tools/realdevice/smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX
```

首次干净环境：

```bash
python3 tools/realdevice/smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX \
  --fresh
```

`--fresh` 会擦除 WiFi/NVS；只有在允许重新配网时使用。

测试完成后应生成唯一 run directory，例如：

```
tools/realdevice/logs/20261008-213000/
  report.md
  report.json
  uart.log
  commands.log
  esptool/
  flash/
  metadata/
```

不要只生成一个平面的 log 文件。

## 6. 自动化测试阶段

### S0 固件与启动

- 确认固件版本/commit SHA。
- 记录 ESP-IDF、Python、Node、esptool 版本。
- 记录 USB port、device IP。
- 必要时烧录 full image。
- 非 fresh 模式只清动态池和 carve store，不擦 WiFi/NVS。
- 捕获启动 UART。
- 确认服务可访问。

PASS：

- 无 panic/assert/watchdog/reboot loop。
- HTTP service 正常。

### S1 配对/会话

- 从 UART 捕获 pair ready。
- POST pair。
- 校验 token。
- 后续优先从持久化 NVS 恢复 token，避免每次人工按键。

### S2 首次 APP + DATA

- 从真实 `/api/install/slots` 获取 listing。
- 必须调用仓库中的 Node dynslot allocator，而不是 Python 重写算法。
- prepare。
- 校验 carve proposal。
- 上传 APP。
- 上传 DATA initial image。
- DATA 必须先上传 prefix，再查询 status，确认 offset checkpoint。
- 只上传 suffix。
- 查询 DATA done。
- finalize。

PASS：

- APP offset 正确。
- DATA offset 从 prefix 长度继续。
- DATA final offset == initialImageSize。
- DATA done=true。
- finalize 200。
- reboot 后 slot count=1。

### S3 第二槽

- 第二次安装。
- slot count 从 1 → 2。
- 重启后仍为 2。
- 不允许第一槽或 DATA 被破坏。

### S4 ARCHIVED + eraseData

- 删除带 DATA 的旧 slot。
- slot count 正确下降。
- DATA 进入 ARCHIVED。
- 再删除剩余 slot 并请求 eraseData。
- 删除后 count=0。
- 记录 DATA 生命周期状态。

### S5 中断续传

必须制造真实传输中断：

1. prepare。
2. APP 只上传约 1/3。
3. 主动关闭连接/停止上传。
4. monitor 停止。
5. esptool `run` 软复位。
6. monitor 恢复。
7. 服务恢复。
8. 查询 session/status。
9. 重发 prepare 必须幂等。
10. 从已有 offset 继续。
11. finalize。
12. reboot。
13. 验证 slot/data state。

注意：软复位只能证明 RAM 丢失后的恢复；它**不是严格等价于物理断电**。报告必须把二者区分。若要完成真正 power-loss 验收，需要 USB 供电控制器/继电器或人工断电测试。

### S6 Flash 取证

自动读取：

- partition table。
- carve store。
- DATA extent。

至少对 DATA initial image 做 byte-for-byte comparison：

```
read_flash(DATA.offset, len(initial_image))
==
initial_image
```

同时计算 SHA-256。

如果 metadata 正确但 bytes 不一致，优先归因 storage write/offset/erase，而不是 HTTP 层。

## 7. 失败处理

smoke 失败后不得只输出“FAIL”。

必须：

1. 保存异常 traceback。
2. 保存 UART tail/full log。
3. 保存最近一次 HTTP request/response 摘要，但不要泄露 token。
4. 保存 slots/status JSON。
5. 保存已能安全读取的 partition/store/data dump。
6. 保存执行命令及退出码。
7. 给失败阶段编号。
8. 给机器可读的 failure category。

建议 category：

- ENV_MISSING
- USB_PORT
- USB_BUSY
- FIRMWARE_FLASH
- BOOT
- HTTP
- AUTH
- CAPACITY
- CARVE
- APP_UPLOAD
- DATA_UPLOAD
- DATA_RESUME
- DATA_FINALIZE
- ARCHIVE
- ERASE
- REBOOT_RECOVERY
- FLASH_VERIFY
- POWER_LOSS_UNTESTED
- UNKNOWN

## 8. 归因规则

按证据优先：

- Host test fail → 代码逻辑/测试。
- Firmware build fail → 编译、链接或 size。
- simulator boot fail → image / partition / bootloader。
- simulator pass + real boot fail → 硬件、真实 Flash、ROM/外设差异或 image 烧录。
- API 正常 + upload fail → installer protocol/storage path。
- DATA metadata 正确 + Flash bytes 错 → DATA write/offset/erase。
- 正常 reboot fail → durable carve record / boot recovery / runtime table。
- reboot pass + real power-loss fail → power-loss ordering / durability。
- USB 无日志 → cable/driver/port/monitor ownership。
- panic/watchdog → firmware runtime。
- 只有 fresh pass → persistent-state/migration/reclaim。
- 所有自动化 PASS 但 power-loss 未执行 → 不能宣称 P0-5 真机完全验收。

## 9. 报告要求

`report.json` 至少包含：

- timestamp
- git_commit
- git_branch
- firmware_sha256
- idf_version
- host/os
- python/node/esptool versions
- serial_port
- device_ip（可脱敏）
- fresh
- stages[]
- overall_status
- failure_category
- artifacts[]

每个 stage：

```json
{
  "name": "S2",
  "status": "PASS",
  "started_at": "...",
  "ended_at": "...",
  "checks": [
    {"name": "data_resume", "status": "PASS", "details": "..."}
  ]
}
```

`report.md` 要给人读，包含：

1. 环境。
2. 固件 commit。
3. 测试阶段。
4. PASS/FAIL。
5. 失败归因。
6. 关键 UART。
7. Flash verification。
8. artifacts 路径。
9. 未覆盖项。
10. 对代码修改的建议。

## 10. Agent 工作纪律

读取本 handoff 后：

- 先检查当前 `feat/storage` 是否有未提交修改。
- 不在 main 开发。
- 先执行现有测试。
- 再执行 simulator。
- 有真机则执行完整 smoke。
- 失败必须先收证据和归因，再改代码。
- 改代码必须补/修改自动化测试。
- 每次修改后重新跑受影响层。
- 不得把“代码已写好”当成“真机通过”。
- 不得把 soft reset 写成 power-loss PASS。
- 最终输出必须包含真实执行过的命令、结果、报告路径和未完成项。

## 11. 当前已知代码状态

`tools/realdevice/smoke.py` 已经覆盖：

- APP + DATA。
- DATA resume checkpoint。
- DATA done idempotency。
- ARCHIVED DATA。
- DATA Flash byte comparison。
- reboot / resume 基础链路。

但它还需要继续完善：

1. per-run artifact directory。
2. report.json/report.md。
3. 所有 stage 的显式 PASS/FAIL 状态。
4. failure category。
5. 命令 stdout/stderr 落盘。
6. failure 时自动采集 slots/status。
7. token 脱敏。
8. 明确区分 soft reset 与真实 power loss。
9. Flash dump 与 report 的关联。
10. 测试结束后给出可直接交给 coding agent 的诊断摘要。

这些不是可选优化，而是为了让真机失败能够真正反哺代码开发。

## 12. 最终完成定义

只有以下条件全部满足，才能说本轮真机验收完成：

- L1 Host PASS。
- L2 passport-sim PASS。
- L3 real-device smoke S0-S6 PASS。
- DATA Flash byte comparison PASS。
- reboot recovery PASS。
- report + logs + flash evidence 完整。
- 没有未解释的 panic/reboot/HTTP/storage failure。

如果没有真实 power-loss 设备控制能力，必须明确标记：

`POWER_LOSS_UNTESTED`

而不能把它隐含为 PASS。
