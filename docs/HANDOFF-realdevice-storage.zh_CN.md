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
4. APP 卸载时 APP + 全部关联 DATA 级联删除。
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

## 3. passport-sim 回归测试（可选，不再作为当前合并阻塞）

passport-sim 仍然是有价值的 boot/runtime 回归层，但本轮最终固件已经完成真实 ESP32-C3 真机 S0-S6 验证，因此**不得因为未在最终固件提交上重跑 simulator 就阻塞合并**。

如果本地 agent 具备 simulator 环境，仍建议执行：

```bash
PASSPORT_SIM_DIR=../passport-sim ./tools/validate.sh --sim
```

若执行失败，按 image / boot / display / CPU / button / TEST_HARNESS 分类并保留日志；只有证据指向 meta-pass 且影响本轮改动时才修改代码。

本轮已知事实：历史 simulator PASS 可以作为背景证据，但如果不是针对最终固件提交重新执行，不得写成“最终提交 simulator PASS”。

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

### S4 APP + DATA 级联删除

- 删除一个带多个 DATA label 的 APP。
- 设备端一次性枚举该 APP 的全部 DATA；UI 不提供单独删除 DATA 或保留 DATA 的选项。
- 删除后 APP 与全部关联 DATA 记录均不存在，关联 extents 不再被引用，池可用空间变化与释放范围一致。
- 其他 APP/DATA 的记录与字节保持不变。
- 若擦除/持久化提交任一步失败，不得返回成功，也不得提前回收仍被引用的 extent。
- 中断删除并重启，持久化事务幂等恢复；已删除 APP 不得被镜像扫描复活。
- 在真实字节级备份完成前，UI 必须警告卸载会永久删除数据，不能声称可备份/恢复。

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

## 11. 最终代码与测试结果审核——本地 Agent 必做

当前最终真机验证对应提交：

`5eed4b87a984a88485509a4ac2a0d577a506e55e`

本地 agent 在继续修改前必须先完成以下审核：

1. **检查最终 diff**：比较 `90c66f3` 与最终提交，确认所有修改都与 storage 设计目标或测试可靠性直接相关，没有无关改动。
2. **检查 carve slot 字段所有权**：重点审计 `meta_carve_slot_t` 以及 `meta_carve_flash_sync_states()`。runtime table 重建只能重新计算 runtime 派生字段；`play_id` 等 durable record-owned 字段必须从持久化 carve state 保留。不得再次出现“重建 runtime state 把 durable metadata 清零”的问题。
3. **检查回归测试**：确认 `tests/test_meta_carve_flash.c` 中的 `sync_states/play_id` 回归测试确实覆盖该 bug，而不是只验证正常路径。
4. **检查报告可追溯性**：`docs/storage-test-report-20261008.zh_CN.md` 应以最终实际测试提交 `5eed4b8...` 作为 firmware/git commit；不能把最终固件描述成仅仅“在旧 commit 工作树上测试”。这是证据链问题，不是功能问题。
5. **检查三处已修复问题**：HTTP status JSON 截断、`sync_states` 丢失 `play_id`、S6 esptool read length 十六进制参数。确认每一处都有对应测试或真实证据，并没有回归。
6. **检查最终 CI**：最终提交的 static/firmware workflow 应为成功状态。
7. **不要重复无价值的整套真机测试**：如果审核没有发现新的代码问题，不需要因为 simulator 未重跑而重新执行 S0-S6；已有最终固件真机证据应直接复用。
8. **真正未完成的 P0-5 证据是 physical power loss**：soft reset 只证明 RAM loss 后持久化状态恢复，不能替代物理断电。若本地没有可控断电设备，明确保留 `POWER_LOSS_UNTESTED`，不要修改代码伪造 PASS。

审核输出必须回答：

- 是否发现新的代码缺陷？
- 是否需要补测试？
- 是否需要修改设计/实现？
- 最终提交、firmware SHA-256、CI、真机 S0-S6 是否一致可追溯？
- physical power loss 是否仍未测试？

只有发现具体缺陷时才进入“改代码 → 补测试 → 重跑受影响层”；否则进入 merge recommendation。

## 12. 当前代码状态

`tools/realdevice/smoke.py` 已覆盖：

- APP + DATA 安装。
- DATA resume checkpoint。
- DATA done 幂等跳过。
- ARCHIVED DATA 生命周期。
- DATA Flash 字节级比对。
- reboot / resume 基础链路。

`tools/realdevice/run_smoke.py` 已提供本地验收包装层，负责：

1. 每次运行独立 evidence directory。
2. 保存命令、stdout/stderr、UART 与 HTTP observations。
3. 记录 git branch/commit、固件 SHA-256、工具版本、USB port/IP。
4. 生成 `report.json` 与 `report.md`。
5. 失败时输出 failure category，并保留诊断证据。
6. 明确区分 soft reset 与真实 physical power loss。

当前已完成的最终固件真机证据包括 S0-S6、DATA Flash byte comparison 和 soft-reset recovery；最终提交为 `5eed4b87...`。

仍需区分两类证据：
- passport-sim：可作为可选回归测试；未针对最终固件提交重跑时，不得声称最终提交 simulator PASS。
- physical power-loss：仍未执行则必须标记 `POWER_LOSS_UNTESTED`。

因此，当前本地 agent 的重点从“继续盲目跑测试”转为“完成最终 diff / 字段所有权 / 回归测试 / 报告可追溯性审核”；只有发现具体问题才继续改代码。

## 13. 最终完成定义

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

## 14. 2026-10-10 新增验收要求：每次安装必须新建 APP carve

当前 feat/storage 已调整为严格的新安装语义：

- 手机安装页只显示动态回收池为本次 APP 计算的新 carve 提案；不允许选择已有空槽/有效槽/无效槽原位覆盖。
- 无法读取 /api/install/slots 或设备不支持动态池时，安装页必须 fail closed，不再回退固定三槽安装路径。
- 设备 POST /api/install/prepare 拒绝不带 has_carve 的旧式请求，防止客户端绕过 UI。
- 删除 APP 后，APP carve 记录被移除、对应物理范围返回池；之后即使分配到相同 offset，也必须作为新分配处理。关联 DATA 默认归档保留，不能把 APP 槽位释放等同于 DATA 擦除。
- 同一 prepare 的网络重试仍应命中幂等分支，不应重复分配；只有本次未完成安装创建的 carve 才能在失败清理中回收。

**重要：此前真机 S0-S6 证据并未覆盖这项新策略，不能直接视为本次策略已验收。** 本地 Agent 必须在此改动的最终 commit 上重新执行静态/宿主测试与固件构建；有真机时新增至少以下断言：

1. 首装 A：槽位数量 +1，APP offset 与设备 allocator 的 fresh proposal 一致。
2. 安装 B：再新增一个槽位，A 的数据与运行状态不受影响；不得覆盖现有 empty/valid/invalid 槽。
3. 删除 A：A 的 APP carve 被释放，删除的镜像不能重新启动；关联 DATA 按归档策略保留。
4. 再安装 C：从当前回收池重新计算并分配，允许物理 offset 与 A 相同，但槽位是一次新分配，不能恢复旧镜像/旧状态。
5. 动态槽位清单不可达或旧固件不支持 carve 时，UI 和设备 API 都拒绝安装，不得走 legacy 原位安装。
6. 对不带 has_carve 的直接 HTTP prepare 请求验证返回 400 fresh APP carve required，且 carve 记录、分区表和既有 APP/DATA 均不改变。

这项改动改变了原有“优先安装到现有空槽”的行为，因此必须重新跑受影响的 CI 和真机路径；如果当前环境没有真机，明确记录为未验证，不得用旧报告代替。
