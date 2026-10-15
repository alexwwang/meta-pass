# 真机测试报告 | USB DATA 备份读取(Stage A–C,门禁关闭)

语言:[English](usb-data-backup-read-test-report-2026-10-14.md)

- 日期:2026-10-14
- Handoff:`docs/HANDOFF-usb-data-backup-read.md`
- 测试分支/commit:`feat/storage` @ `ec7d956`(拉取后的实际 HEAD,已记录)
- 结论:**FAIL** —— Stage C 备份完整性未达成;Stage D 按计划未执行。

## Stage A —— 主机与模拟器(PASS)

- `./tools/validate.sh` 全量:PASS,含固件构建 `meta-pass_v2.1.0-1109-gec7d956.bin`。
- `./tools/validate.sh --sim`(passport-sim):PASS(5.1s)。

## Stage B —— 环境冻结

- 固件 SHA-256:`377aefcf…8dcdb`(factory 0x10000+0x170000,esptool read-flash,与此前会话一致)
- 主机:macOS(darwin 24.6.0),Chrome 155.0.8059.40,Node v26.3.0,esptool 5.4.0,ESP-IDF v5.5.3
- 串口 `/dev/cu.usbmodemXXXX`(脱敏),设备 192.168.0.14,页面 `dev-v2.1.0-1109-gec7d956 · chunk=32KB stop-and-wait baud=921600(fallback 115200)`
- 基线为空设备;Play 28 组(APP `REC-P28-HUMAN-GATE` slot 0 @1572864+1462272,play_id 2147483648;DATA `recordings` @3538944,4841472B)由恢复此前已验证的归档重新建立(`?noverify`,47.0s,写路径不变)。已知良好 DATA 载荷哈希 `cba83ea5…`。
- 环境发现:拉取前的 dev server 进程静态白名单陈旧(新的 `flash-readback-verify.js` 模块 404,页面无法就绪)——已用当前 checkout 重启;早前一次 `esptool read-flash` 曾把芯片留在 bootloader 模式(RTS 脉冲复活,无数据影响)。

## Stage C —— 只读 DATA 备份:两次自然复现,均 FAIL

每次尝试一个全新页面会话,页面 `readFlashChunked`(32KiB stop-and-wait)@115200,Debug 开,不做重连/重载/分段规避。

| | 尝试 1 | 尝试 2 |
|---|---|---|
| 成功读 / 字节 | 89 / 2,895,872 | 44 / 1,441,792 |
| 死亡偏移 | 0x4c8000(DATA 内约 1.47MB) | 0x490000(APP 槽读刚结束) |
| 首错 | `Packet content transfer stopped` | `Packet content transfer stopped` |
| 页面 resync(排空 803ms → sync) | 每次重试 `No serial data received` | 相同 |

- 死亡前逐读耗时平坦(32KiB 中位 ~2,853ms)——传输突然死亡,无劣化。esptool-js TRACE 尾部:`Read 254 bytes: (空)`、`Read 77 bytes: (空)`,随后 `Packet content transfer stopped`(stub 在包中途停止发数据)。
- **"~30 次 readFlash 调用"的刻画对只读备份流量不成立**:两次会话分别死于第 45、90 次成功读;历史 ~30 次观察(带前置写流量的 restore 路径)落在该散布区间内。
- **对照实验(归因)**:浏览器释放串口后,esptool.py 在同线同波特率(115200)读完整 DATA 区间 `0x360000 + 4841472B`,**21.7s 零错误**,SHA-256 `cba83ea5…` = 已知良好载荷。设备、线缆、波特率、flash 均健康 → **失败在浏览器侧(Chrome WebSerial / esptool-js 传输)**。

## 门禁决定

两次尝试均未产出完整归档,载荷哈希验证无从执行;按计划,Stage D(删除、恢复、逐字节比较、重启持久性)**未执行**。设备上 APP+DATA 组原样保留(每次尝试后经 LAN 验证;死备份会把芯片留在 stub 模式,均以 RTS 脉冲复活)。

## 未做事项(按指示)

未改分块大小、未加第二读取器、未做页面重载分段、未用 `flashMd5sum` 替代、未手动擦除分区、未执行删除/恢复阶段。

## 随本次入库的工具

- `tools/realdevice/usb_data_backup_read_once.mjs` —— Stage C 自然复现驱动(UI 驱动单次尝试,逐读日志实时捕获,失败分类 `USB_READ_TIMEOUT|USB_READ_PROTOCOL`,成功时做离线归档验证)。

## 证据(本地,gitignored)

`tools/realdevice/logs/20261014T005837Z-usb-data-backup-read/` —— `report.md`/`report.zh_CN.md`/`report.json`、`environment.txt`、`attempt1-*`/`attempt2-*`(逐读页面日志、esptool-js TRACE console、终态 JSON、截图、解析读统计)、`esptool-read-data.log` + `esptool-data-0x360000.bin`(对照)、`restore-rundir/`(建组,恢复 PASS 47.0s)。

## 后续(同日经用户批准已执行)

**Stage D 经分段路线完成**(`p28_chunked_backup.mjs`,已入库;仅备份环节分段,其余全部正常生产路径):

| 步骤 | 结果 |
|---|---|
| 分段备份(每 512KB 一个全新页面会话 @115200) | PASS 602.7s —— 归档 6,325,885B,载荷经共享模块验证,页面恢复预检接受 |
| 页面 UI 删除 APP | PASS 22.5s —— DATA 级联删除确认,无孤儿 DATA |
| 生产路径恢复(**设备侧 MD5 读回**,上游新机制) | PASS —— 提交 + 两个区间 MD5 校验 + 复位后可见;harness 旧字节读回检查撞上已知主机传输死亡(属重复验证,如实记录) |
| 逐字节比较(esptool 只读 dump,handoff D.5 允许) | PASS —— DATA `cba83ea5…`、APP `f519f076…` 均与归档载荷一致 |
| 重启持久性 | PASS —— 复位及后续 RTS 重启后组均完好 |
| 清理 | 回基线 `count=0 free=6766592`(harness exit=1 系合成 run-state 缺 `baselineFree`,LAN 已验证) |

**总结论**:页面单会话备份在此类主机上无法完成(Chrome WebSerial 传输死亡,已复现 4 次并归因);但 备份→删除→恢复→字节比较→重启持久 全链功能端到端正确,且设备侧 MD5 读回在真机工作正常。


