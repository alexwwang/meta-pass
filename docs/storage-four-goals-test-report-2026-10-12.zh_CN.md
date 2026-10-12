# Storage 四项功能真机测试报告

简体中文 | [English](storage-four-goals-test-report-2026-10-12.md)

日期：2026-10-12
执行：本地 Agent（无人工按键/读屏，全部经 LAN API、USB Serial/JTAG 测试控制通道、Chromium CDP）
基线分支：`feat/storage` @ `302c466`（+ 修复提交 `e472d24`，本报告的代码变更随该提交）
配套 handoff：`docs/assets/storage-four-goals-handoff.zh_CN.md`

## 0. 结论速览

| 四项功能 | 结论 | 关键证据 |
|---|---|---|
| 1. 手机内嵌页与 USB 服务页对齐 | 部分通过 | DATA 容量选项已共享（`data-size-options.js`）；两页独立 UI/流程未统一（handoff 已声明）。手机页 smoke 修复后 **15/15 PASS**；USB 多轮 E2E **BLOCKED**（见 §5） |
| 2. 保留 `cardid`/`store` 的分区容量方案 | 通过 | 真机分区几何与池模型一致；固定分区未动；单测（JS/C 黄金对拍）全绿 |
| 3. 专用测试子固件验证 DATA 读写 | **通过（真机）** | A/B 双固件：BOOT→ERASE→WRITE→READ→校验和→子固件 REBOOT→重读一致→返回 launcher，全部 PASS（`runtime-A.json`/`runtime-B.json`） |
| 4. 安装时创建 DATA 分区（完整生命周期） | **通过（真机）** | 装 A/B→归属核对→隔离核对→卸载 A（B 数据不受影响）→删后不可启动→卸载 B→**基线逐字节恢复** |

**本轮回合共发现并修复 5 个真机阻断级缺陷**（§4），其中 2 个为 bootloader/固件级（httpd handler 溢出、e2edata 白名单不一致、data-child 控制台、冷启动子固件引导断裂——实为 4 个固件级 + dev server 白名单 + E2E 门断言漂移）。

## 1. 被测版本与制品

- 被测 commit：`e472d24`（含 `302c466` 全部历史；修复内容见 §4）
- 构建：launcher `SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.e2e-test.defaults`（`CONFIG_META_E2E_TEST_CONTROL=y`，仅测试构建）；data-child 独立工程
- ESP-IDF：v5.5.3（本地）；芯片 ESP32-C3（设备标识已脱敏）
- 固件 SHA-256（被测构建）：
  - launcher app：`b3b42ea7b6f99abfa4dc46a44615f591038694fa4fc6adcfe09421bf3f065b8b`
  - bootloader：`1d05821b697ed2f8460034c84edfe583d0a852c349748284bb4eb5354a706ba8`
  - data-child app：`ada014490741e2bda89d4d691c714d8da54e8b443314eb4608d944c6b9aec2e3`
  - data-child 合并镜像（安装输入）：`e772794e8a29c6e5d2dbf273dac0cb5e4399b9597861bc3eacc5d4ac4bba74da`
- 设备端版本串：`v2.1.0-1003-g302c466-dirty`（+ e472d24 工作树构建）

## 2. 自动化检查（handoff §2）

| 检查 | 结果 | 说明 |
|---|---|---|
| `./tools/validate.sh --static` | **PASS（278 项）** | 含修改后的契约/单测（meta_boot_policy 新增 is_boot_request 断言等） |
| `./tools/validate.sh --all` | 部分 | Host 单测 PASS；固件部分因 shell 无 IDF 环境失败一次（记录原因后于 IDF 环境补跑 `./tools/validate.sh --firmware`，1200s 超时中断于临时构建目录——三个目标全量构建超出时限） |
| CI Static checks @302c466 | success | run `38208263541` |
| CI Firmware checks @302c466 | success | run `38208263557`（7m33s） |
| e472d24 推送后 CI | 见仓库 Actions | 推送后由 CI 兜底 |

## 3. 执行过的自动化命令（退出码与日志）

| 命令 | 退出码 | 结果/日志 |
|---|---|---|
| `node tools/realdevice/mobile_page_browser_smoke.mjs --url http://127.0.0.1:4191/m --cdp-url http://127.0.0.1:9222` | 0 | **PASS 15/15**（修复前 5/15 FAIL：`/m` 页 3 个 JS 模块 404）；`tools/realdevice/logs/mobile-browser-smoke-2026-10-12T05-38-24-165Z/` |
| `idf.py build`（launcher，e2e overlay） | 0 | 构建+反复烧录验证 |
| `idf.py -C tests/realdevice/data-child build` | 0 | data-child 重建（含控制台修复） |
| `node tools/realdevice/local_data_child_install.mjs --ip <device-ip> --play-id 990001/990002 ...` | 0 | A/B 各一次成功安装（中间两次失败为修复前状态，见 §4）；日志 `tools/realdevice/logs/local-data-child-install-*/` |
| `python3 tools/realdevice/mobile_page_runtime_driver.py --action boot-test-and-return --slot 0/1 ...` | 0 | A/B 全项 PASS；`tools/realdevice/logs/four-goals-2026-10-12/runtime-{A,B}.json` |
| `python3 tools/realdevice/mobile_page_runtime_driver.py --action verify-deleted --play-id 990001 ...` | 0 | 删除后 launcher 拒绝启动旧 play_id；`verify-deleted-A.json` |
| `POST /api/install/remove`（slot 0 ×2） | 200 | 卸载 A、卸载 B；快照见 `four-goals-2026-10-12/` |
| `node tools/realdevice/usb_web_e2e_multi.mjs --real-device --play-a 28 --play-b 582 ...` | 1 | E0/E1 **PASS**（修复后 idle 门可满足），R1 BLOCKED（Web Serial 授权缺失，§5）；`tools/realdevice/logs/usb-web-e2e-multi-20261012T071900Z/` |

原始证据目录：`tools/realdevice/logs/four-goals-2026-10-12/`（基线/过程/终态快照、UART 引导日志、运行时证据 JSON）。token、设备 IP/MAC/串口标识已脱敏，未写入任何提交。

## 4. 真机发现与修复（全部随提交 `e472d24`）

### F1 — install 服务 httpd handler 槽溢出（阻断级）
- 现场：刷机后 LAN 安装服务不可达；boot log `httpd_uri: no slots left for registering handler` → `install service resume failed`。
- 根因：本分支新增 `/api/install/nvs/info` + `/api/install/nvs/read` 后注册 17 个 URI > `max_uri_handlers=16`，末位注册失败令整个 `httpd_start` 失败回滚（2026-10-11 同类事故注释犹在：`13 > 12 时末位注册失败会令整个 install 服务启动失败`）。
- 修复：`main/meta_store_install.c` 16→20 并更新注释。

### F2 — C 侧 DATA subtype 白名单缺失 e2edata(0x40)（阻断级）
- 现场：专用子固件 prepare 被 400 `carve proposal rejected (label=e2edata)` 拒。
- 根因：JS 分析器 `parseFirmwareManifest` 接受 `subtype 0x40 && label=="e2edata"`（仅测试 fixture），而 C 侧 `meta_install_model_place_offer` 与 `meta_carve_valid` 只认 0x81/0x82/0x83 —— handoff §5 明确警告的"JS/C 分配器必须同步修改"类缺陷。
- 修复：`main/meta_install_model.c`、`main/meta_carve.c` 白名单镜像 JS 规则（0x81/0x82/0x83 + 仅 `e2edata` 的 0x40）。

### F3 — data-child 无控制台（阻断级）
- 现场：BOOT_SLOT 后主机永远等不到子固件 `READY`。
- 根因：`tests/realdevice/data-child/sdkconfig.defaults` 设 `CONFIG_ESP_CONSOLE_NONE=y`、`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n`，而行协议（READY/HELLO/INFO/WRITE/READ/ERASE/REBOOT）全部跑在 USB Serial/JTAG 控制台上。
- 修复：启用 `USB_SERIAL_JTAG` 控制台。重建后 `data_child.bin` SHA 由 `ee3427…` 变为 `ada014…`（证明生效；此前一次"重建 SHA 不变"暴露 build 目录缓存陈旧，已清理重建）。

### F4 — 冷启动子固件引导链路断裂（阻断级，bootloader）
- 现场：`E2E BOOT_SLOT`/`esp_ota_set_boot_partition` 返回 OK（otadata 写入回读验证 `seq=1 state=NEW` 正确），但重启后 bootloader 打印子固件视图分区表后仍 `Defaulting to factory image`。
- 根因：bootloader hook 的 carve 门只把**深睡唤醒**的 PENDING 当作子固件会话（`active_slot`）；冷启动一律按 launcher 视图裁决 —— 子固件视图表被回写、otadata 两扇区被清（`meta_carve_boot_restore` 设计如此）。而本分支 UI 启动与 e2e 通道全部走 `esp_restart`（冷启动），**不存在深睡引导路径**——子固件从此永远无法引导。这是上游深睡修复（r10.34）移植到本分支后的集成断裂。
- 修复：`meta_boot_policy.h` 新增 `meta_boot_policy_entry_is_boot_request`（`state==NEW && seq 非空`），hooks.c 冷启动路径新增 `peek_boot_request_slot` 镜像深睡 peek 语义；单测补断言（`tests/test_meta_boot_policy.c`）。冷启动 PENDING 仍回 launcher（单次会话模型不破）。
- 注：只刷 app 不刷 bootloader 时旧 hook 会把含 e2edata 的 carve 记录判非法并回安全表——**app/bootloader 必须同刷**，已用全量重刷验证 A/B 槽位跨重启存活。

### F5 — dev server 白名单缺 3 个模块（测试基础设施）
- 现场：`/m` 手机模块页 smoke 5/15 FAIL（`data-size-options.js`、`data-byte-archive.js` 404 导致模块图加载失败）；`install-slot.html` 静态 import 的 `data-install-plan.js` 同样 404。
- 修复：`tools/install-slot/server.mjs` 白名单补三员；重启服务后 smoke **15/15 PASS**。

### F6 — usb_web_e2e_multi idle 门永不满足（测试契约漂移）
- 现场：E1 `assertInstallerIdle` 要求 `active=false`，而 `active` 由 `token_start` 置位、httpd 存活即恒真——服务必须在线才能被读取，门自相矛盾（2026-10-09 多次失败同因）。
- 修复：在途判定改为 `session/offer/confirmed`；修复后 E0/E1 PASS。

### 测试通道与工具补全（同提交）
- `meta_e2e_control.c`：新增 `E2E INSTALL_STOP`（产品 `meta_install_net_stop` 同款收会话，恢复列表页冷态）；`E2E SESSION_RESET` 补拉起 install 服务与 STA（INSTALL_STOP 后服务/网络自恢复）。
- `tools/realdevice/local_data_child_install.mjs`：复用页面模块（`extractAppImage`/`extractDataImages`/`geomFromListing`/`runInstall`）的本地安装驱动。市场安装类 E2E 无法安装专用子固件（无市场条目），该驱动走与页面完全相同的设备安装事务，是本轮真机证据的产生工具。

## 5. 真机生命周期矩阵（handoff §4，逐项）

| 场景 | 结果 | 证据 |
|---|---|---|
| 安装含 DATA 的 APP（×2） | PASS | 槽0 DCH-A @0x180000+176128，DATA e2edata @0x1B0000+64KiB，play_id 990001；槽1 DCH-B @0x1C0000+176128，DATA @0x1F0000+64KiB，play_id 990002（`local-data-child-install-*` 日志） |
| APP 与 DATA 同一 play_id 归属 | PASS | 设备 `/api/install/slots` 清单逐条核对 |
| DATA 容量 4KiB 对齐/低于镜像要求拒绝 | PASS（单测） | `validate.sh --static` 分配器/契约测试 |
| 最大连续区间不足拒绝并报告两种容量 | PASS（单测+UI 逻辑） | `dynslot-pool.js` 诊断路径；真机池 free=6766592（两段几何与 handoff §0.2 一致） |
| DATA 写入/读回摘要错误不发布所有权 | PASS | runtime driver 校验 nonce/SHA-256/CRC32 全等才置位 |
| 安装后重启 APP/DATA 归属与字节保留 | PASS | `dataPersistedAfterReboot=true`（A、B 各自，经真实 bootloader 重启+子固件重读） |
| 取消卸载不变 | N/A（本轮无 UI 取消路径自动化） | — |
| 卸载 APP 仅删关联 DATA | PASS | 卸载 A 后 B 的 APP/DATA 原样存活，A 的 DATA 记录消失，free +237568 精确释放 |
| 备份（manifest+载荷 SHA-256） | NOT RUN | USB 多轮 E2E（覆盖备份路径）BLOCKED 于 Web Serial 授权 |
| 恢复 | NOT RUN | 同上；恢复仍为纯预检计划（handoff 已声明未实现端到端） |
| v1 旧归档标识 / MPTB v2 fail-closed | PASS（自动化） | 静态套件恢复事务测试 PASS 1-10 |
| 删除后不可启动 | PASS | `verify-deleted`：launcher 对旧 play_id 990001 回 `SLOT_NOT_BOOTABLE` |
| 卸载后基线恢复 | PASS | 终态与测试前基线**逐字节一致**（`final-baseline.json` == `baseline-slots.json`，free=6766592，count=0，archived=0） |

deviceRecovery.status = **RESTORED**（独立读回 + 基线比对一致）。

## 6. 重启前后 SHA-256 对照（DATA 记录级）

| 固件 | nonce | 写入后 SHA-256(前 16) | 重启重读 SHA-256 | 一致 |
|---|---|---|---|---|
| A（slot0/990001） | `…`（证据内） | 见 `runtime-A.json` | 见 `runtime-A.json`（`dataPersistedAfterReboot=true`，含 CRC32） | ✓ |
| B（slot1/990002） | `53d4687eda31…` | `d78f987ba545fa96…` | 同左 | ✓ |
| B 在 A 删除后 | 同上 | 同上 | `b-survival-after-a-delete.json` `matchesPreDeletion=true` | ✓ |

对照口径为子固件读回记录的 SHA-256/CRC32（主机独立复算一致），非文件级镜像哈希。

## 7. BLOCKED / NOT RUN 及原因

| 项 | 状态 | 原因与下一步 |
|---|---|---|
| `mobile_page_e2e.mjs`（市场安装路径） | BLOCKED | handoff 要求的"两个已发布的专用测试 play 条目"不存在：市场搜不到任何 data-child/e2e 条目，既有 play 28（录音笔）/582（坑道守听）均为业务固件，不说 e2edata 行协议，runtime 步骤必失败。发布需市场管理权限。已用本地安装驱动 + runtime driver 覆盖同等真机生命周期（§5）。 |
| `usb_web_e2e_multi.mjs` R1-R4 | BLOCKED | E0/E1 已修复并通过；R1 `uiConnect` 需 profile 内**恰好一个**已授权 Web Serial 端口，实测 0 个。授权需人工在原生端口选择器点选一次（本轮尝试 AppleScript 驱动系统对话框超时，放弃）。下一步：人工跑一次 `node usb_web_e2e.mjs --authorize` 后重跑。 |
| Play 28 录音业务 E2E | NOT RUN（保持 BLOCKED） | 按 `mobile-page-data-e2e-market-firmware.zh_CN.md`：需该玩法录音 UI 的 DOM/导出接口与版本固化，本轮未补齐，状态不变。 |
| 备份/恢复端到端 | NOT RUN | 备份路径在 BLOCKED 的 USB E2E 内；恢复实现仍为预检计划（handoff 已声明，未以文案冒充完成）。 |
| `./tools/validate.sh --firmware` 本地全量 | 未完成 | 1200s 超时（三目标全量构建）；CI Firmware checks @302c466 成功，e472d24 以推送后 CI 为准。 |

## 8. 分区布局评估（handoff §5）

真机分区表（bootloader 打印）与 handoff §0.2 一致：固定区 `cardid@0x356000+0x4000`、`store@0x35A000+0x6000` 未动；动态池 `0x180000..0x356000` + `0x360000..0x7FE000` 两段；安装后 carve/对齐/gap 由设备分配器裁决，A/B 提案几何与设备复核一致（carveOffset/carveSize 逐位通过 `meta_install_model_carve_ok`）。未做任何跨段拼接。

## 9. 未完成项与下一步

1. 市场侧发布两个专用测试 play 条目（指向 data-child 合并镜像），解锁 `mobile_page_e2e.mjs` 全链路。
2. 人工授权 Web Serial 端口后重跑 `usb_web_e2e_multi.mjs`（E1 门已修复），覆盖备份与重启持久性。
3. e472d24 推送后盯 CI Static/Firmware；本地 `--firmware` 建议在有 CI 兜底的前提下不重复跑全量。
4. Play 28 业务 DATA 验收按 market-firmware handoff 补齐前置后单独立项。
