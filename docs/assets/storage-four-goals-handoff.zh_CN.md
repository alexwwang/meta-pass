# Storage 四项功能交接：实现验收与真机验证

简体中文 | [English](storage-four-goals-handoff.md)

日期：2026-10-14  
仓库：`alexwwang/meta-pass`  
工作分支：`feat/storage`  
PR：[#1](https://github.com/alexwwang/meta-pass/pull/1)  
目的：让本地 Agent 在不改 `main`、不合并 PR 的前提下，复现自动化测试、完成指定专用测试设备上的真机验收，并提交带原始日志的结果。

## 0. 重要结论

本交接文档用于追踪剩余验收项，不应覆盖后续更新的真机报告。每项结论以最新代码、自动化结果和带日期的真机报告为准；旧报告中的 NOT RUN 不能在新证据出现后继续沿用。

1. **手机内嵌页与 USB 服务页对齐（未完成）**：新 v2 语义改为整设备已分配存储快照，不再是 APP+DATA 逻辑归档。手机页已有快照备份和归档/目标布局预检，但恢复不会写 Flash，因为事务化写入与失败回滚协议尚未实现。USB 保留固定槽设备的历史 v1.1 备份/还原；动态 v2 备份目前 fail-closed，拒绝继续生成旧 APP+DATA v2 归档；USB 也拒绝旧 APP+DATA v2 归档和新快照实际写入。USB 仍独有本地固件安装入口。两页的 v2 快照写入/还原尚未完成，不能宣称已对齐。
2. **保留固定 `cardid`/ `store` 的分区容量方案**：绝不移动固定分区或直接改动 `cardid`。动态池被固定区域切成两个物理区段；单个连续 DATA reservation 不能跨越固定分区。当前 `dynslot-pool.js` 的池段为 `0x180000..0x356000` 和 `0x360000..0x7fe000`，第二段约 4.61 MiB。不能仅凭两个区段的总空闲容量声称 5.7 MiB DATA 可安装。若市场镜像声明的最小 DATA 大于最大可分配连续区间，必须明确拒绝，并同时展示最大连续空闲空间与总空闲空间；手机页和 USB 页已补充该诊断。除非设计并实现了设备端兼容的非连续 DATA 映射，否则不能把一个 reservation 跨段拼接。
3. **专用测试子固件验证 DATA 读写**：使用 `tests/realdevice/data-child/`，经 USB Serial/JTAG 的受控命令写入测试记录、读回并校验、重启后再次校验。只允许通过分区标签 `e2edata` 解析分区，不接受任意 Flash 地址。市场玩法不支持这个串口协议，不能拿它代替专用测试固件。
4. **安装时创建 DATA 分区**：安装前校验最小容量、4 KiB 对齐、记录数和连续空间；安装后核对 APP 与 DATA 的 `play_id` 归属；读写、重启持久性、卸载释放和重新安装都要覆盖。现有分配器/安装计划代码和部分真机安装证据，不等于完整生命周期已通过。

## 1. 工作区与安全约束

```bash
git fetch origin
git switch feat/storage
git status --short --branch
git log -1 --oneline
```

- 保留用户已有未提交改动；不使用 `git reset --hard`、不强推。
- 不合并 PR，不在 `main` 开发。
- 真机写操作仅限明确指定的专用测试设备，且设备上没有需保留的数据。
- 不使用市场固件执行破坏性测试，不在未知设备上自动卸载/擦除。
- token、配对码、真实设备 IP、串口敏感标识不得写进日志或提交。
- 每轮测试保存 JSON/文本原始报告、完整命令（移除秘密）和测试固件 SHA-256。

## 2. 自动化检查（先执行）

```bash
./tools/validate.sh --static
./tools/validate.sh --all
```

若 `--all` 因未安装 ESP-IDF 失败，记录确切失败原因；不得把 `--static` 的通过写成固件构建通过。检查 CI 最新 commit 的 Static checks 与 Firmware checks，记录链接与结论。

## 3. 页面自动化

### 3.1 手机页面无写操作 smoke

启动项目现有开发服务与本机 Chromium CDP 后运行：

```bash
node tools/realdevice/mobile_page_browser_smoke.mjs \
  --url 'http://127.0.0.1:4191/m' \
  --cdp-url 'http://127.0.0.1:9222'
```

检查所有视口、横向溢出、console error、失败网络请求与必需控件。不要用 USB 页 `/?mock=1` 替代手机模块壳页 `/m`。

### 3.2 手机存储管理真机 E2E

先阅读 `docs/assets/mobile-page-storage-local-test-handoff.zh_CN.md` 和 `docs/assets/mobile-page-data-e2e-test-firmware.zh_CN.md`。准备专用测试固件、串口、Chromium CDP、pyserial，并确认 E2E 控制配置启用。随后按既有 handoff 的命令运行：

```bash
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device \
  --url "$MOBILE_E2E_URL" \
  --cdp-url "$MOBILE_E2E_CDP_URL" \
  --runtime-driver tools/realdevice/mobile_page_runtime_driver.py \
  --require-data-reservation \
  --play-a <专用测试APP的play-id> \
  --play-b <另一个专用测试APP的play-id>
```

根据当前脚本 `--help` 核实参数是否需要从环境变量读取；不要把 token 直接放到 shell history。基线安全门必须通过，否则停止，不得继续写设备。

### 3.3 专用 DATA 子固件串口闭环

先构建专用固件，确认烧录的是 `tests/realdevice/data-child/` 对应的测试镜像，而非市场玩法。按 `tools/realdevice/data_child_serial.py --help` 查看当前协议/参数，再执行 HELLO、INFO、写测试记录、读回校验、重启、再次读回校验。每个阶段保存机器可读结果。

验收要求：
- INFO 返回标签 `e2edata`、大小及分区信息；
- 写入返回成功，设备读回记录 magic/version/nonce/SHA-256/CRC 全部有效；
- 重启后记录仍存在且摘要一致；
- 记录只能写入已解析的 DATA partition 内，测试脚本不传任意绝对 Flash 地址；
- 记录清理只擦除专用测试 DATA 的首 sector，且必须再次确认目标设备与分区标签。

若测试固件安装后无法启动，停止并收集串口启动日志；不要通过移动 `cardid` 或改分区表来绕过问题。

### 3.4 USB 服务页面 E2E

运行现有 USB E2E，不另造一套测试框架：

```bash
node tools/realdevice/usb_web_e2e.mjs --help
node tools/realdevice/usb_web_e2e_multi.mjs --help
```

先读 help 和脚本中的设备保护条件，按仓库既有参数运行。覆盖安装、DATA 归属、备份、卸载、重启后清单持久性及空间释放。不要假定手机 E2E 通过即代表 USB E2E 通过。

## 4. 必须逐项覆盖的生命周期矩阵

| 场景 | 期望结果 | 证据 |
|---|---|---|
| 安装不含 DATA 的 APP | 成功，不凭空创建 DATA | 安装报告 + carve 前后快照 |
| 安装含 DATA 的 APP | 成功，DATA 与 APP 使用同一 play_id 归属 | APP/DATA 清单 JSON |
| DATA 容量非 4 KiB 对齐/低于镜像要求 | 写入前拒绝 | 单元测试 |
| 总剩余容量足够但最大连续区间不足 | 写入前拒绝并报告两种容量 | 单元测试 + UI 截图/日志 |
| DATA 标签/子类型冲突、记录槽耗尽 | 写入前拒绝，设备状态不变 | 单元测试 + carve diff |
| DATA 写入/读回摘要错误 | 不发布 DATA 所有权，失败状态明确 | 故障注入或可重复模拟测试 |
| 安装后重启 | APP 和 DATA 所有权及数据字节保留 | 重启前后清单与 SHA-256 |
| 取消卸载 | 槽表、DATA 和空闲空间均不变 | 前后快照 |
| 卸载 APP | 仅删除关联 DATA；空间可重用；其他 APP 不受影响 | carve 前后快照 |
| v2 快照备份 | 手机页完整读取所有允许分区，manifest 与所有非擦除 extent 的长度/SHA-256 相符；USB 动态模式当前 BLOCKED | ZIP 校验报告 + 页面日志 |
| v2 快照恢复 | 当前仅支持归档/布局预检；事务化擦写、回滚与写后读回尚未实现，禁止执行 Flash 写入 | 预检报告；恢复写入状态必须标 BLOCKED |
| v1.1 旧归档 | 固定槽设备保留历史 APP/NVS 备份与恢复；动态 v2 不生成旧 APP+DATA 逻辑归档 | UI/契约测试 |
| v2 整设备快照 | 手机页可备份并做恢复预检；USB 动态快照备份、两页快照写入/回滚均 BLOCKED，未实现前不得声称完成 | 快照契约测试 + 后续真机证据 |

**新快照功能状态（2026-10-14）：** 旧的 `docs/backup-restore-test-report-2026-10-13.zh_CN.md` 与 `docs/usb-backup-segmented-session-test-report-2026-10-14-round2.zh_CN.md` 验证的是此前 APP+DATA 逻辑归档流程，不能作为新整设备快照恢复的证据。当前 `install-slot/storage-snapshot.js` 提供快照 ZIP、payload SHA-256 校验和非变更式恢复计划；`main/meta_store_install.c` 仅提供受保护的分区清单/分区相对偏移读取 API；手机 UI 可生成快照并预检归档，但不会执行写入。USB 动态模式拒绝生成旧 APP+DATA v2 归档，USB 动态快照备份尚未接入，因为串口页无法可靠取得设备报告的当前运行宿主分区。两端均未实现快照写入/事务回滚，故恢复预检不等于恢复。旧 v1.1 固定槽路径继续保留。下一阶段先完成安全的事务化恢复/回滚协议和 USB 宿主分区识别，再运行新快照真机测试。

## 5. 分区布局评估要求

不得只比较 `POOL_TOTAL`。对每种目标镜像至少输出：
- 各固定分区的起止地址（来自真实设备分区表）；
- 各动态池区段的起止地址；
- 已占用 carve、DATA reservations、对齐后剩余 gap；
- 最大连续可分配空间与总空闲空间；
- 目标 APP 槽和 DATA 分配后的模拟结果；
- 失败是否由连续空间、槽位数、DATA 记录数或镜像声明最小值导致。

任何改动必须同时修改/验证 JS 分配器与 `main/meta_carve.c` 的 C 分配器黄金向量。没有设备端兼容协议和真实读写证据时，不得把 DATA 分区跨过 `cardid` 拼成单一分区。

## 6. 报告与交付

将报告写到 `docs/storage-four-goals-test-report-<日期>.zh_CN.md`，包括：
1. 被测 commit SHA、固件 SHA-256、设备型号/芯片与 IDF 版本（移除可识别设备标识）；
2. Static/Firmware CI 链接与结果；
3. 每个自动化命令、退出码、测试数和原始日志路径；
4. 四项功能分别给出 PASS / FAIL / BLOCKED / NOT RUN，并引用代码/测试路径；
5. 真机操作前后 APP 槽、DATA reservation、分区几何、空闲容量快照；
6. 重启前后 SHA-256 对照；
7. 失败现场、恢复动作与恢复后的独立读回证据；
8. 明确未完成项与下一步，不以“测试框架存在”代替功能通过。

本地 Agent 只负责测试、补充测试/报告并提交到 `feat/storage`；不要合并 PR。