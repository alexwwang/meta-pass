# Handoff｜USB DATA 备份读取与 APP+DATA 恢复真机测试

## 1. 当前代码与 CI 基线

- 仓库：`alexwwang/meta-pass`
- 分支：`feat/storage`，不要在 `main` 上开发。
- 待测基线：`5f785f5637f1a342e82ff41a7d497d93022eb1fd`（测试开始时先执行 `git rev-parse HEAD`；若 HEAD 已前进，报告实际 commit，并确认本次代码仍包含本 handoff 对应的读取实现）。
- 已核实该 commit 的 GitHub Actions：
  - Static checks #1069：SUCCESS，run ID `38458135700`。
  - Firmware checks #1041：SUCCESS，run ID `38458135940`。
- 该 CI 结论只适用于上述 commit。它不证明 Chrome/Web Serial 的 DATA 字节备份或真机恢复已经通过。
- 当前 USB 页面 APP/DATA 备份都走既有 `readFlashChunked()`；分块为 32 KiB，最多重试 8 次。该读取函数及 `vendor/esptool-js.js` 的 `readFlash()` 与 v1.1 对应实现一致。本轮不引入第二套读取框架。
- 已知待复现现象：Chrome/macOS Web Serial 读取约 4.6 MiB DATA 时，曾在约 30 次 `readFlash` 调用后卡住；同一设备上 `esptool.py` 以 921600 baud 读取约 4 MiB 约 18 秒。以上是历史线索，必须在本轮重新采证。

## 2. 测试顺序与安全条件

1. 先运行当前 checkout 的 host/simulator 测试；再运行只读 DATA 备份；只有备份完整性 PASS 后才允许删除 APP+DATA。
2. 不移动 `cardid`，不硬编码 Flash 地址，不绕过 allocator，不改变现有分区布局。
3. 不能以 ZIP 下载成功、APP 镜像存在、设备侧 `flashMd5sum` 成功替代 DATA 原始字节备份。
4. 不进行物理断电测试；串口复位不等同于断电。
5. 归档可能包含原始 NVS/Wi-Fi 凭证，不要上传公共服务或提交到 Git。测试报告必须脱敏 token、Wi-Fi 凭证和其他秘密。

## 3. 阶段 A：本地代码与模拟器验证

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
git rev-parse HEAD
./tools/validate.sh
PASSPORT_SIM_DIR=../passport-sim ./tools/validate.sh --sim
```

如果项目约定的 simulator 命令在当前 checkout 有差异，以 `tools/validate.sh` 和现有文档为准，记录实际执行命令与退出码。若 simulator 不存在或无法启动，报告为 `ENV_MISSING` / `SIM_BOOT_FAILED`，不能记作 PASS；继续真机前先确认设备固件构建与安装方式安全。

## 4. 阶段 B：记录固定测试环境

尽可能使用此前失败报告中的相同 Mac、Chrome、ESP32-C3 Passport、USB 数据线和波特率策略。记录：
- branch/commit、固件 SHA-256、ESP-IDF/esptool/Python/Node 版本；
- macOS 与 Chrome 完整版本、USB 串口端口、设备 IP；
- USB 页面 URL、页面资源对应的 commit、实际连接波特率；
- 当前 APP/slot/DATA listing；DATA 的 label、play_id、offset、size；
- 测试固件/应用名称、DATA 实际字节数。

先保存设备 listing，不要先删除、重分区或刷入其他固件。读取地址和长度必须取自当前设备 API/分区记录，不能复制历史设备地址。

## 5. 阶段 C：DATA 备份读取（破坏性操作之前）

1. 打开 USB 服务页，连接设备并启用页面 Debug mode；保留页面日志、DevTools Console，最好录屏并记下时间。
2. 选择一个拥有 DATA 且 `play_id` 稳定的 APP 组，执行一次备份。动态模式下每个 v2 归档只备份一个 APP+DATA 组。
3. 不要同时运行占用该串口的 monitor/esptool。观察每个读取块的地址、长度、开始/结束时间、重试/恢复日志、总读取字节数。
4. 保存 ZIP 到本地测试证据目录，并离线验证：
   - ZIP 可完整解压；manifest 为 v2 单组归档，`complete=true`；
   - 没有 `missing_slots` / `missingSlots`；
   - 每个 DATA reservation 均有实际 payload，payload 长度符合 manifest `bytes`，SHA-256 与 manifest 一致；
   - 页面没有 DATA read error、metadata ownership changed、archive incomplete 等错误。
5. 记录 DATA 字节数、耗时、`readFlash` 调用次数、重试数、最长调用耗时和最后一条日志时间。

**阶段 C PASS**：以上条件全部成立。ZIP 能下载但 DATA payload 缺失或哈希不符，必须判 FAIL，不可继续删除。

若再次在约 30 次读取后卡住：
- 不要先改分块大小或连续反复操作；等待至少 60 秒，记录最终状态；
- 保存完整页面日志、Console 导出、异常栈、最后 100 条协议/串口日志（若可用）、最后一次读取的 offset/length、已完成字节数；
- 区分页面仍可交互、明确报错、Promise 长时间不返回、整页无响应；
- 归类 `USB_READ_TIMEOUT`、`USB_READ_PROTOCOL`、`PAGE_HANG` 或 `UNKNOWN`；
- 失败时停止，不执行 APP/DATA 删除。

## 6. 阶段 D：APP+DATA 删除、恢复与持久性（仅阶段 C PASS 后）

1. 将完整 ZIP 复制到证据目录，重新验证 manifest 与全部 payload SHA-256。
2. 通过 USB 页面正常删除对应 APP，确认所属 DATA 随 APP 一并删除且无孤儿 DATA 记录；不要手工擦除分区。
3. 通过 USB 页面从刚才的 v2 ZIP 恢复该单个 APP+DATA 组。
4. 记录恢复后 APP/DATA listing，包括 play_id、label、offset、size 与事务状态。
5. 使用只读 Flash dump 做 DATA byte-for-byte 比较。地址和长度必须来自恢复后的设备 API/分区记录；记录 dump SHA-256 与归档 payload 比较结果。如果无法安全 dump，标记为未完成，不要用 metadata 或 MD5 代替。
6. 重启设备，确认 APP/DATA 仍存在，应用能读取原有标记数据。若测试固件支持 USB 命令读写 DATA，执行写入→读取→重启→再读取的持久性验证。
7. 检查其他 APP/DATA 记录与字节未被改变。

**阶段 D PASS**：删除后 APP 与关联 DATA 都消失；恢复后 APP/DATA 关联正确，DATA 逐字节匹配，重启后仍可读，其他组不受影响。

## 7. 只读对照与归因

在浏览器测试完成、释放串口后，可用 `esptool.py read_flash` 对相同 DATA 范围进行只读对照，记录命令、波特率、耗时、文件 SHA-256；不要同时占用串口。

- esptool PASS、浏览器 FAIL：优先检查 Web Serial/esptool-js transport、ACK/响应等待与调用状态。
- 浏览器备份成功但 payload hash 不符：检查地址/长度、读取返回字节、archive payload 映射。
- 设备 API 元数据异常：先定位 allocator/carve metadata，不要归因于读取协议。
- 恢复后 metadata 正常但字节不符：定位 DATA 写入、offset、erase 或 restore transaction。
- 两种读取都失败：检查硬件、串口、Flash 状态与设备固件，并保留证据。

不要在没有可复现证据时更改 32 KiB 分块、增加新的读取抽象、用 `flashMd5sum` 代替原始 DATA 读取，或声称简单 disconnect/connect 已证明 transport 被干净重置。

## 8. 交付报告

建立 `tools/realdevice/logs/<timestamp>-usb-data-backup-read/`，至少包含：
- `report.md`、`report.json`、`environment.txt`；
- 页面日志、Console 日志、执行命令与退出码；
- ZIP 归档、manifest 摘要、各 payload SHA-256；
- 备份/恢复前后设备 listing；必要的只读 Flash dump 与 hash；
- 失败时的异常栈、最后成功 offset、失败分类和根因证据。

`report.json` 至少包含：`git_commit`、`git_branch`、`device_firmware_sha256`、`os`、`chrome_version`、`serial_port`、`baud`、`data_bytes`、`read_calls`、`read_retries`、`backup_duration_seconds`、`archive_sha256`、`payload_hashes_match`、`restore_byte_compare`、`reboot_persistence`、`overall_status`、`failure_category`、`artifacts`。

请明确区分 simulator、浏览器原始字节备份、删除/恢复、重启持久性、物理断电（本任务未执行）的状态。真机失败应先交付证据与归因，不要直接试探性修改 Flash 读取协议。
