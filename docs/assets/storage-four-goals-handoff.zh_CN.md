# Storage 四项功能交接：实现验收与真机验证

简体中文 | [English](storage-four-goals-handoff.md)

日期：2026-10-14  
仓库：`alexwwang/meta-pass`  
工作分支：`feat/storage`  
PR：[#1](https://github.com/alexwwang/meta-pass/pull/1)  
目的：让本地 Agent 在不改 `main`、不合并 PR 的前提下，复现自动化测试、完成指定专用测试设备上的真机验收，并提交带原始日志的结果。

## 0. 重要结论

本交接文档用于追踪剩余验收项，不应覆盖后续更新的真机报告。每项结论以最新代码、自动化结果和带日期的真机报告为准；旧报告中的 NOT RUN 不能在新证据出现后继续沿用。

1. **手机内嵌页与 USB 服务页的 v2 快照路径已接入代码，待自动化/真机验证**：两页均使用整设备已分配存储快照，不把 APP+DATA 逻辑归档误认成 v2 快照。USB 继续保留固定槽设备历史 v1.1 归档恢复，并为动态布局提供 v2 快照备份/顺序恢复；手机内嵌页只接受 v2 快照。恢复统一采用完整预检、逐分区擦除/写入、独立全分区读回核验，不实现事务日志、自动回滚或复杂修复编排。USB 仍独有本地固件安装入口。当前真机验证尚未完成，不能仅凭 host mock 断言设备实际操作已通过。
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

新增 v2 快照恢复验收（必须使用可擦写的专用测试设备，禁止在有需保留数据的设备上执行）：

1. 先确认设备运行的 MetaPass 版本和当前分区表；使用 USB 页备份完整 v2 快照，安全保存 ZIP 与 SHA-256。
2. 在不改变分区布局的前提下，通过专用测试 DATA 玩法写入并核验一个唯一测试标记；随后用快照恢复覆盖该变化。
3. 恢复日志必须包含预检、分区擦除/写入、独立读回阶段；读回阶段覆盖每个目标分区的全范围（包括应为 0xFF 的稀疏区域）。恢复完成后，再次读取测试 DATA，确认字节与快照时一致。
4. 手动退出下载模式/重启设备，确认 MetaPass 可重新连接，再次检查分区清单与测试 DATA。把数据读回 PASS 和重启/重连 PASS 分开记录。
5. 使用不匹配的 MetaPass 版本或故意改变分区几何进行预检负例；必须在任何 Flash 写入前失败。记录操作前后可观察的目标数据摘要，证明无写入。
6. 不执行断电中断测试，不要求验证自动回滚。若擦除/写入/读回失败，停止后续操作，记录分区标签、相对偏移和错误，并按提示刷回快照来源 MetaPass 版本后重试。

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
| v2 快照备份 | 手机页与 USB 动态模式读取所有允许分区；manifest 与非擦除 extent 的长度/SHA-256 相符，记录来源 MetaPass 版本和保守兼容范围 | ZIP 校验报告 + 页面日志 |
| v2 快照恢复 | 完整预检后逐分区擦除/写入；每个目标分区独立读回全范围比对，稀疏区期望为 0xFF；核验后再单独验证重启/重连 | 单元测试、页面日志、真机报告；失败须记录分区与相对偏移并建议刷回归档来源版本 |
| v1.1 旧归档 | 固定槽设备保留历史 APP/NVS 备份与恢复；动态 v2 不生成旧 APP+DATA 逻辑归档 | UI/契约测试 |
| v2 整设备快照 | 两页都接入快照备份与顺序恢复代码；USB 保留 v1.1 兼容，手机页不处理 v1；自动化/真机仍需验证 | 快照契约测试 + 后续真机证据 |

**新快照功能状态（2026-10-15）：** 新归档是当前设备已分配存储的原始字节快照；清单携带来源 MetaPass 版本及兼容版本范围。当前保守策略把兼容范围限定为来源版本本身；没有有效版本元数据时拒绝生成/恢复。设备端提供分区清单、分区相对读、分区擦除和分区相对写 API，并拒绝写入 cardid、运行中的宿主分区及只读分区。恢复先校验 ZIP、所有 payload SHA-256、版本范围和当前分区几何，随后逐分区擦除、写入 extent，再独立读回整个分区（包括稀疏的 0xFF 区域）。不做事务日志、自动回滚或复杂修复。写入/读回失败会报告分区与相对偏移并提示刷回归档来源 MetaPass 版本后重试；数据核验通过不等于设备已成功重启，当前页面需要用户退出下载模式/重启并确认可重新连接。USB 通过 ESP app descriptor 识别唯一 MetaPass 宿主分区；若不能唯一识别则 fail-closed。真机恢复与重启验证仍未完成，不能把 mock 测试当作真机证据。