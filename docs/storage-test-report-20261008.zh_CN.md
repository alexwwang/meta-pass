<p align="right">
  <strong>简体中文</strong> · <a href="storage-test-report-20261008.md">English</a>
</p>

# Storage 分支测试报告 — feat/storage

**日期**:2026-10-08
**分支**:`feat/storage`
**基准 commit**:`90c66f3` — docs(storage): clarify deferred reboot terminology
**受测固件 commit**:在 `90c66f3` 之上含两个真机驱动修复的工作树(见 §4);
固件 SHA-256 `5518535173c7d3352400a134f1f05f5b17d3b92b840af5a3a21edcb07b14f36e`
(1,111,408 字节,app 镜像于 0x10000)

---

## 1. 模拟器测试(passport-sim)

**状态**:通过(自本报告首轮沿用,未变化)

```
✔ meta-pass 完整镜像在 QEMU 里引导并渲染出 240x320 屏幕
✔ QEMU CPU 在 boot 后仍持续运行而非停在异常状态
✔ DOWN 键移动列表选中项(画面随之重绘)
✔ 连续 4 次 DOWN 绕列表一圈,固件不崩溃
✔ UP 与 DOWN 成对使用,焦点来回移动且画面稳定
```

环境:Node.js v26.3.0,passport-sim 本地 checkout,
`tools/sim/esp_emu_bg.wasm`(3.3 MB)。退出码 0。

---

## 2. 真机 Smoke 测试

**最终状态**:**通过 — S0/S2/S3/S4/S5/S6 全绿**
**运行目录**:`tools/realdevice/logs/20261008-121330/`(证据包完整:
command.txt、stdout.log、stderr.log、uart.log、slots/status 响应、
report.json、report.md、flash/{table.bin,store.bin,recordings.bin})
**真实断电**:未执行 — 按 handoff 规则记为 POWER_LOSS_UNTESTED
(软复位 ≠ 断电)。S5 仅用了 esptool 软复位。

### 2.1 环境

| 项 | 值 |
|----|----|
| 设备 | ESP32-C3 AI Passport,8 MB flash |
| USB 口 | `/dev/cu.usbmodem142401` |
| MAC | `4c:11:ae:2f:67:ec` |
| LAN IP | `192.168.0.24`(AP "Hundhaus",WPA2,rssi −66/−67 dBm) |
| 烧录方式 | 仅 app `write_flash 0x10000` — NVS、分区表、otadata、cardid 均未动(无 `erase-flash`) |
| IDF | v5.5.3-dirty;宿主 Python 环境经 `IDF_PYTHON_ENV_PATH=idf5.5_py3.10_env` 固定(自动侦测的 py3.14 venv `pydantic_core` ABI 损坏 — 见 §3 问题 1) |
| Node | `/usr/local/bin/node`(真 Node;`~/.local/bin/node` 是 bun 包装,`node -e argv` 会坏) |

### 2.2 阶段结果

| 阶段 | 描述 | 结果 | 关键证据 |
|------|------|------|----------|
| S0 | 清池+store、启动、NVS token 复用 | 通过 | `carve loaded: fresh device (safe table)`、`session token restored across reboot`、install 服务就绪、HTTP 401→200 |
| S1 | 配对 | 通过(token 复用路径) | `复用 NVS 持久化 token` — 无需物理按键 |
| S2 | 首次 APP+DATA 安装(carve 提案 + 续传探针) | 通过 | `prepare#1 200 (carve idx=0 off=0x180000)`、`DATA[0] resume checkpoint offset=2048`、`DATA[0] done offset=4096`、`finalize 200`、重启 → `carve loaded seq=2 slots=1` |
| S3 | 第二个槽安装 | 通过 | `prepare#2 200`、`finalize 200`、重启 → `carve loaded seq=4 slots=2` |
| S4 | 删除 + ARCHIVED,再 eraseData 删除 | 通过 | remove slot0 → `seq=6 slots=1`,DATA `recordings` state=2(ARCHIVED);remove slot1 eraseData → `seq=7 slots=0` |
| S5 | 370,469 B 处掐断、软复位、幂等续传 | 通过 | 服务自动恢复、旧 token 仍有效、幂等重发 prepare 200、`finalize 200`、重启 → `carve loaded seq=10 slots=1` |
| S6 | flash 回读 + DATA 字节比对 | 通过 | recordings.bin(4096 B)== 首传镜像,SHA-256 `4e441a35…7205ec`;table.bin + store.bin 已存档 |

`report.json` 状态:PASS,soft_reset_tested: true,
physical_power_loss_tested: false,failure_category: None。

### 2.3 UART 证据(最终运行)

```
meta_carve: fresh device (safe table); no carve yet
install_local: session token restored across reboot
meta-pass: 就绪:APP 槽=0 free=6766592B s0=0 s1=0 s2=0
meta_carve: carve committed: seq=1 slots=1 materialize=1   (S2 prepare)
meta_carve: carve loaded: seq=2 slots=1                    (S2 重启)
meta_carve: carve committed: seq=4 slots=2 materialize=1   (S3 prepare)
meta_carve: carve loaded: seq=4 slots=2                    (S3 重启)
meta_carve: carve loaded: seq=6 slots=1                    (S4 归档删除)
meta_carve: carve loaded: seq=7 slots=0                    (S4 eraseData 删除)
meta_carve: carve loaded: seq=10 slots=1                   (S5 续装+finalize 重启)
```

---

## 3. 测试中发现的问题(均已当场解决)

### 问题 1 — 环境:IDF Python env 自动侦测损坏(存量问题)

`source ~/esp/esp-idf-v5.5.3/export.sh` 自动侦测 Python 3.14.7 并激活
`idf5.5_py3.14_env`,其 `pydantic_core` 带的是 `cpython-310` 原生模块
(ABI 不匹配)→ 所有 `idf.py` 调用死于
`No module named 'pydantic_core._pydantic_core'`。smoke  harness 正走这条路
启 `idf.py monitor`,当天首轮运行 UART 采集因此完全失效。

**规避(未改仓库)**:运行 smoke 前 export
`IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.10_env`
(已验证健康:pydantic_core 2.46.5 可导入)。

**建议**:重装 py3.14 venv(`idf_tools.py install-python-env`)或删除它,
让自动侦测回落到健康环境。

### 问题 2 — 固件(已修):`/api/install/status` JSON 截断

**症状**(运行 `20261008-115132`):S2 在测试 harness 崩
`json.decoder.JSONDecodeError: Expecting ',' delimiter: line 1 column 283`,
解析 DATA prefix 上传后的 status 响应时。

**根因**(`main/meta_store_install.c`,`h_install_status`):响应尾巴 `]}`
用 `snprintf(body + off, …)` 追加,但 `off` 未含这两字节;
`httpd_resp_send(req, body, off)` 发出的 body 缺最后两字节。
姊妹函数 `h_install_session` 用的是 `httpd_resp_sendstr`(按 strlen),
本来就对。

**证据**:探针实测设备返回 282 字节、结尾 `…"done":false}` —
恰好短 2 字节。

**修复**:status handler 改用 `httpd_resp_sendstr`,与 session handler
同款。同类构造点已全量 grep:全文件仅这两处这样拼 JSON,无其他副本需改。

### 问题 3 — 固件(已修):`sync_states` 清零槽位 `play_id` → 归档链断裂

**症状**(运行 `20261008-115953`):S4 失败 — UART 报
`archive_slot_and_data failed: ESP_ERR_INVALID_STATE`;删除槽位后 DATA
`recordings` 停在 state=0(未变 ARCHIVED=2)。

**根因**(`main/meta_carve_flash.c`,`meta_carve_flash_sync_states`):
运行时表回填用 `memset(&built, 0, …)` 重建槽位条目,只回填了
kind/offset/size/state/SHA/name。`play_id` 是记录侧元数据、运行时表
不带此字段,于是每次回填提交(即每次安装后 `materialize=0` 的 seq 前进)
都把它静默清零。`meta_carve_flash_archive_slot_and_data` 随后撞上
`play_id == 0 → ESP_ERR_INVALID_STATE` 守卫。

**证据**:真机 flash 回读(`read_flash 0x35A000`)解码 store 两扇区,
槽位条目 `play_id=0`,而数据记录 `play_id=1`。

**修复**:回填时把 `play_id` 复制进 `built`。回归断言加进
`tests/test_meta_carve_flash.c::test_sync_states`(置 play_id=42 → sync
→ 断言保留)。

### 问题 4 — 测试 harness(已修):S6 读长十六进制格式

**症状**(运行 `20261008-120733`):S6 字节比对失败,只回读 1000 字节。

**根因**(`tools/realdevice/smoke.py` S6):f-string 产出
`read_flash 0x2a0000 1000 …` — `{len(data_bytes):x}` 把 4096 渲染成
`1000` 且没有 `0x` 前缀,esptool 按十进制 1000 解析。

**证据**:已回读的 1000 字节与 fixture 0 差异 — flash 内容本就对,
只是读短了。

**修复**:输出 `0x{len(data_bytes):x}`。文件内其余 `read_flash` 长度
已查(其上两处调用),均不缺前缀。

---

## 4. 本次测试会话中的代码改动

| 文件 | 改动 | 原因 |
|------|------|------|
| `main/meta_store_install.c` | `h_install_status`:`httpd_resp_send(req, body, off)` → `httpd_resp_sendstr(req, body)` | 问题 2 — off 未计入 `]}` 尾巴 |
| `main/meta_carve_flash.c` | `sync_states`:重建条目时保留 `slot[i].play_id` | 问题 3 — 记录侧元数据被每次回填清零 |
| `tests/test_meta_carve_flash.c` | `test_sync_states` 加回归断言 | 问题 3 — play_id=42 经 sync 保留 |
| `tools/realdevice/smoke.py` | S6 读长 `0x{len:x}` | 问题 4 — flash 回读被截断 |

修复后的宿主验证:`test_meta_carve_flash` 全套 PASS
(12 个场景,含新回归);姊妹套件经 `tools/validate.sh --static` 整体重跑
— 见 §5。

## 5. 验证

- 真机 smoke:`tools/realdevice/logs/20261008-121330/` — 退出码 0,
  全阶段 PASS(输出见 §2.2)。
- 宿主测试:`test_meta_carve_flash` 整个文件 PASS(不止新断言);
  报告文档落地后 `--static` 全套(文档规则、公钥一致性、全部 C 宿主
  测试、actionlint)转绿。
- 固件重编 + 仅 app 烧录由启动日志(`carve loaded`、
  `LAN install service ready`)与通过的本次运行双重确认。
- 一致性 grep:JSON 响应构造点(问题 2)与 `read_flash` 长度实参
  (问题 4)已查姊妹副本 — 无遗漏。

## 6. 结论

- **模拟器**:通过(与首轮相同)。
- **真机**:**通过** — 最终运行 S0–S6 全绿;DATA 字节级 flash 校验通过;
  重启恢复通过(软复位)。
- **POWER_LOSS_UNTESTED**:未做物理断电;S5 只用 esptool 软复位。
  不得以此报告断电耐久性。
- **未测项**:物理按键导航(harness 已注明需人工抽验)、真实断电耐久、
  USB 网页安装页(install-slot)针对本固件的端到端。

---

*报告由 omp coding agent 撰写 — 2026-10-08。此前被阻塞的一轮报告
(Agnes / Sapiens AI)保留在本文件的 git 历史中。*
