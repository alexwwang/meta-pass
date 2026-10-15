# v2 整机存储快照备份/还原 —— 真机测试报告(2026-10-15)

语言:[English](storage-snapshot-restore-test-report-2026-10-15.md)

## 结论

**修复后 PASS。** handoff(`HANDOFF-realdevice-storage.zh_CN.md` §19–20)的验收项在修复后的代码树上全部达成:USB v2 快照备份/还原 PASS、手机内嵌页 v2 快照还原 PASS、USB v1 兼容回归(host 层)PASS;每次还原后均完成重启 + 重连验证,并有逐字节证据。测试同时暴露并修复了 `b74e9da` 上的三个真实缺陷。

## 环境

- 被测 HEAD:`b74e9da`(v2.1.0-1475)**叠加工作区修复**(固件报 `v2.1.0-1475-gb74e9da-dirty`)。
- 设备:ESP32-C3 AI Passport,8MB Flash;烧录 e2e 测试全量镜像(`build-e2e/FoloToy-AI-Passport-full-e2e.bin`,`CONFIG_META_E2E_TEST_CONTROL=y`),修复后重编产物 sha256 `6dd5f281…`。
- 全量烧录擦除 WiFi 凭证后,以最小 NVS 镜像(命名空间 `metapass`,键 `sta_ssid`/`sta_pass`)写入 0x9000 完成配网;经 `E2E SESSION_RESET` 免按键配对(token 已脱敏,存 `/tmp/token.txt`)。
- 主机:macOS 15.7.4、Chrome 155.0.8059.40、Node v26.3.0、esptool 5.4.0、ESP-IDF 5.5.3。

## 本轮发现并已修复的缺陷

1. **快照静默漏掉全部 child DATA 分区(严重)。** launcher 运行态的物化分区表只给*活动*子固件物化 DATA 分区(`meta_pt_from_carve_active` 设计),两个快照 inventory 都只枚举该表,导致 child DATA 永远不会进快照。真机实证:写入并经设备读回校验的 DATA 记录(nonce 记录,sha256 `b361df96…`)在备份后被人为破坏,还原未将其恢复,且全程无警告。**修复**:设备侧 `storage/info` 把 carve DATA 记录并入 inventory(完整 label/type/subtype/offset/size 身份,已物化条目自动跳过),`storage/read|erase|write` 在表内无匹配时按 carve 记录身份经 `esp_flash_*` 执行;USB 页 `readUsbStorageInventory()` 同样并入 `slotModel.carve.data`。参考:`dynslot-record.js` 的 `materializeTable(carve)`(本就始终包含 DATA)。
2. **USB 快照页面模块加载失败(dev server)。** `install-slot.html` 导入 `storage-snapshot.js`/`storage-snapshot-restore.js`,但 dev server 静态白名单缺失两者 → 404,页面模块整体失效。已修 `tools/install-slot/server.mjs`。
3. **`assertLoader is not defined`(ReferenceError)。** `readUsbStorageInventory()` 调用了原 `commitCarve` 内部局部函数。已提升到模块作用域。

## 结果(修复后固件,组 = data-child `SNAPSHOT-E2E`,play_id 2147483648)

### USB v2 快照备份(第 3 次,`usb-backup3-fixed/`)

- 生产 `#btn-backup` 单击一次,PASS,36.9s;归档 214,093 字节,sha256 `237b2037…`。
- manifest 分区:nvs、phy_init、ota_0(43 个 extent)、store(3)、otadata、**e2edata(1 个 extent = 0x1b0000 处的 4KiB DATA 记录扇区)** —— DATA 覆盖确认。

### 手机内嵌页 v2 快照还原(`phone-restore-fixed/`)

- 范围:经用户确认,手机页架构上初始化加载与空间管理/安装功能已解耦,本轮只测新开发的备份/还原功能,不回归完整加载/安装流程(经管理面板入口直达被测模块);初始化/配对/列表路径作为前置条件实际走过且正常。
- 先将 ota_0 + e2edata 全量写入 0xAA 垃圾数据;确认对话框"即将按快照覆盖 6 个已分配分区"(修复前为 5 个,新增 e2edata),还原 PASS,6.9s。
- 终态 UI:"快照数据已写入。请重启设备…此步骤尚未自动完成"(`warn`)—— 写入完成 ≠ 启动验证,语义正确;无 `rebootAndReconnect` 适配器,启动验证按 handoff 为人工步骤。
- 人工 DTR/RTS 复位重启 → MetaPass 正常启动、LAN 重连、槽位 listing 完全恢复(count=1,slot0 @1572864+176128,e2edata @1769472+65536)。
- 字节级证据:esptool 只读 dump 0x180000+176128 与 0x1b0000+65536 对照归档载荷 —— **44/44 extent 逐字节一致,含 e2edata DATA 记录**(`t6-phone-restore-fixed-byte-compare.json`)。

### USB 页 v2 快照还原(`usb-restore-fixed/`)

- 同样的垃圾变异 → USB 还原 PASS,10.2s,同样的 warn 终态(待重启验证)。
- 人工复位 → 启动 + 重连正常,listing 恢复。
- **44/44 extent 逐字节一致,含 e2edata**(`t6-usb-restore-fixed-byte-compare.json`)。

### 失败边界观察(修复前,保留为证据)

- 在**不同表形状**上还原快照(卸载组后占位分区回到表内)会在首次擦除前被拒:"snapshot does not cover every eligible allocated partition" —— 任何 Flash 写入前的 fail-closed,符合规格。
- 卸载清理曾短时竞争("pending uninstall cleanup failed; retry" 约 60s);fail-closed 门禁生效,幂等恢复在重启后完成(`carve loaded: seq=3 slots=0`)。
- 修复前证据:含 DATA 记录的快照(第 2 次备份)manifest 中**没有 e2edata**,还原后该分区仍是 0xAA,DATA 记录不可恢复。

### USB v1 兼容回归

- host 测试 `test-data-byte-archive.mjs`、`test-backup-data.mjs`、`test-slot-backup.mjs` 全部 PASS(v1 归档的 DATA 拒收、NVS 载荷路径、v1 还原数据通路契约)。
- v1 真机还原:**NOT RUN** —— 本机为动态模式设备,v1 还原需要固定槽设备。按 handoff 要求:host 测试 + 明确 NOT RUN,不虚报 PASS。

## 未覆盖项

- 物理断电测试(按 handoff 保留 `POWER_LOSS_UNTESTED`,本期不要求)。
- 还原后全分区读回(设计已明确移除;以 esptool 独立 dump 做字节级抽查代替)。
- 手机页备份腿本次未重跑(还原逻辑相同,且手机页模块经本地路由拦截从工作区服务;设备 shell 从 CDN 加载的部署版本落后于仓库 HEAD,已在报告中注明)。

## 证据

`tools/realdevice/logs/20261015-snapshot-restore/`:`environment.txt`、各步 inventory、每次运行的 rundir(`usb-backup`、`usb-backup2`、`usb-backup3-fixed`、`usb-restore`、`usb-restore-fixed`、`phone-restore`、`phone-restore-fixed`,含驱动日志、页面日志、结果 JSON、截图、dump 与 `t6-*-byte-compare.json`)。UART 日志在 `/tmp/uart-*.log`(token 已脱敏)。

## 待提交修复

- `main/meta_store_install.c` —— storage 快照 inventory/read/erase/write 的 carve DATA 身份回退。
- `install-slot/install-slot.html` —— `assertLoader` 提升模块级;快照 inventory 并入 carve DATA。
- `tools/install-slot/server.mjs` —— 快照模块静态白名单。
- `tests/test_storage_snapshot_device_contract.py`、`tests/test_usb_backup_segmented_read.py` —— DATA 合并的契约断言。
- 新增驱动:`tools/realdevice/usb_snapshot_backup_once.mjs`、`usb_snapshot_restore_once.mjs`、`phone_snapshot_restore_once.mjs`。
