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

passport-sim 仍然是有价值的 boot/runtime 回归层，但历史 S0-S6 报告不覆盖当前新增的手机 NVS 备份 API 与 S1b 检查。不得把历史硬件结果当作当前 HEAD 的验收结果；是否运行 simulator 与当前 HEAD 的真机验收分开记录。

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
- 所有 UART 输出保存到 run artifact 目录；持久化日志必须将配对 token 脱敏，内存中的配对流程仍使用原始值。
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

主入口必须使用包装器，以生成独立证据目录和机器可读报告：

```bash
python3 tools/realdevice/run_smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX
```

首次干净环境：

```bash
python3 tools/realdevice/run_smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX \
  --fresh
```

只有在调试 smoke 脚本本身时才直接调用 `smoke.py`；直接调用不会生成完整的 `report.json/report.md`。

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
- NVS_API
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

## 11. 当前分支验收纪律

此前文档中的固定“最终真机验证提交”与 S0-S6 通过声明属于历史证据，不代表当前 `feat/storage` HEAD。当前分支后续增加了恢复事务故障注入、USB v2 归档/NVS 语义、手机端 NVS 只读 API 和真机 S1b 检查；这些新变更必须按当前 commit 单独验证。

本地 agent 开始前必须：

1. 确认工作区干净，记录 `git rev-parse HEAD`、`git status --short` 和固件 SHA-256。
2. 在当前 HEAD 执行 `./tools/validate.sh`，并构建固件；记录真实命令与退出码。
3. 如果设备可用，使用第 5 节的 `run_smoke.py` 执行真机测试。S1b 必须在任何安装/删除操作前通过。
4. 检查报告中的 git commit 与实际工作区一致。若固件由不同 commit 构建，必须分别记录，不能混为同一证据。
5. 失败先收集 stdout/stderr、UART、HTTP 状态和阶段编号，再归因并修复；每次修改后重跑受影响测试。
6. soft reset 不等于物理断电。未执行真实断电时，必须标记 `POWER_LOSS_UNTESTED`。

不得把历史提交的 S0-S6 结果、旧 simulator 结果或 host fake-device 测试写成当前 HEAD 的真机 PASS。

## 12. 当前 smoke 覆盖与新增 NVS 阶段

`tools/realdevice/smoke.py` 在配对后、安装/删除前执行 S1b：

- 未认证的 NVS info/read 请求应返回 HTTP 401。
- 已认证请求读取固定系统 `nvs` 分区信息，并读取首尾小块。
- NVS read 越界返回 416；零长度和超过 4 KiB 的请求返回 400。
- 成功读取响应包含 `Cache-Control: no-store`。
- 日志只记录状态与字节长度，不输出 NVS 原始字节。
- 分区大小必须在 1 到 1 MiB 范围内。

包装器 `tools/realdevice/run_smoke.py` 会生成 `report.json` 与 `report.md`。当前 CI 通过仅证明 CI 覆盖的自动化检查成功；S1b 和备份/恢复在真机上的行为仍需由本地 agent 实际执行确认。

## 13. 当前完成定义

- 当前 HEAD 的 host tests 与 firmware build PASS。
- 当前 HEAD 的真机 smoke 已执行，并且报告中的 S1b 与所需存储阶段结果可追溯。
- DATA Flash 字节比对与重启恢复证据完整。
- 没有未解释的 panic、HTTP、存储或恢复失败。
- 若没有实际执行物理断电，明确保留 `POWER_LOSS_UNTESTED`。

passport-sim 可作为额外回归层；如未对当前提交执行，不得报告为当前提交 PASS。

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


## 15. 2026-10-12 新增：恢复事务故障注入与 finalize 不确定态

`tools/install-slot/test-data-restore-transaction.mjs` 已纳入 `tools/validate.sh`，覆盖：

- APP 写入失败发生在 commit 前：不得进入 commit，必须尝试清理临时 staging。
- commit 请求超时：结果视为不确定，不得清理 staging。
- 模拟设备已完成持久化 commit、但响应在传输中超时：仍不得清理可能已被引用的 extent。
- commit 返回成功后，所有权/几何读回校验失败：不得清理可能已发布的数据，必须保留状态供恢复检查。

此外，DATA ZIP 解析器现在会跟踪每个 local record 的字节范围，拒绝 central directory 中多个条目指向重叠/相同 local record 的归档；`tools/install-slot/test-data-archive-zip.mjs` 增加了 central-offset aliasing 的恶意 ZIP 回归测试。

同时，`restoreDataArchive()` 曾在所有安装失败后无条件调用 `cancel()`；这会把 finalize 已被接受、但设备终态轮询超时的情况错误地当作可安全回滚。当前实现将此类结果标记为 `commitUncertain` 并跳过 cancel；只有设备明确报告终态 `failed` 时，才继续走回滚路径。现在也覆盖 finalize 请求本身抛出传输异常的情况：因为请求可能已抵达设备，同样标记为不确定提交并跳过 cancel，提示先检查设备状态，不应盲目重试恢复。集成测试覆盖“finalize 无确认时不 cancel”“finalize 传输异常不 cancel”和“明确 failed 时允许 cancel”。

这些测试仍不能证明真实 Flash 原子性或物理断电持久性。云端 CI 通过后，本地 agent 仍需在目标固件上检查真实 prepare/session/chunk/finalize 路径；不得把 fake-device 测试写成真机证据。

## 16. 2026-10-12 新增：USB v2 归档范围与 NVS 恢复语义

- 动态存储模式下，USB 备份一次只允许选择一个 APP 槽位，与手机内嵌页的单个 APP+DATA 归档/恢复语义一致；避免生成 v2 多 APP 归档而恢复器拒绝导入。
- v2 归档可以保留并校验自动备份的全局 NVS 字节，但 v2 APP+DATA 恢复不会写入全局 NVS；目标设备当前 NVS 保持不变。手机端恢复成功提示与 USB 恢复预检/结果提示均明确说明此行为。
- 旧版 v1 归档的顺序 APP/NVS 恢复兼容路径保持不变。v2 NVS 字节被验证但不应用，是为了避免在 APP+DATA carve 事务之外覆盖全局 NVS；这不构成 NVS 原子恢复的证明。
- 新增/更新的宿主测试覆盖 USB 单组归档门禁，以及含 NVS 的 v2 归档恢复仍可完成 APP+DATA 事务并保留目标 NVS。真机仍需确认恢复后的 DATA 字节和设备状态。

## 17. 2026-10-12 新增：手机端系统 NVS 备份

- 手机内嵌页现在通过配对 session 认证的 `/api/install/nvs/info` 与 `/api/install/nvs/read` 读取固定标签为 `nvs` 的系统 NVS 分区；客户端不能指定标签或 Flash 地址。
- NVS read 只允许 1–4096 字节的单次请求，并严格检查分区边界；备份由客户端按 4 KiB 分块读取，归档保存为 `nvs.bin`，manifest 记录字节数和 SHA-256。
- 备份 ZIP 未加密；NVS 可能包含 Wi-Fi 凭证及其他设备配置。请勿上传或分享归档。读取仅在有效配对 session、用户明确触发备份且安装事务空闲时进行；HTTP 响应禁止缓存。不要将该接口扩展为未认证或任意分区读取。
- 恢复仍然只校验归档 NVS，不写入目标设备 NVS；该接口只提供只读能力，不提供 NVS 恢复能力。
- 云端 CI 的 host/contract 测试不能证明真机分区标签、分区大小和读取行为。真机验收时应检查 NVS info 返回大小、首尾块读取、越界拒绝、未认证拒绝、备份归档中的 `nvs.bin` 大小/hash，以及恢复后目标设备 NVS 未被写入。

## 18. 2026-10-12 新增：真机 NVS API 安全检查

`tools/realdevice/smoke.py` 的 S1b 现在会在配对后、任何安装/删除测试之前执行：
- 无 token 请求 NVS info/read 均应返回 401。
- 已配对 token 可以查询固定系统 NVS 分区大小，并读取首尾小块；只记录长度和 HTTP 状态，不输出 NVS 原始字节。
- 越界读取应返回 416。
- 成功读取响应必须包含 `Cache-Control: no-store`。
- 只接受 0 到 1 MiB 的合理分区大小；实际硬件的分区大小以接口返回值为准。

运行 smoke 时，S1b 是本轮新增的真机断言；云端 CI 只能验证测试代码/契约，不能代替真机运行。
