<p align="right">
  <strong>简体中文</strong> · <a href="mobile-page-e2e.md">English</a>
</p>

# 手机端设备内嵌网页与子固件生命周期 E2E 测试

目标分支：`feat/storage`。

## 1. 测试目标

这不是只检查网页是否显示、槽位 API 是否返回预期值的测试。测试必须实际通过网页完成安装/删除，并在真机上启动安装的子固件、访问其 DATA 分区，验证数据写入、读回、校验和重启后的持久性。

网页通过 CDP 直接控制启用远程调试的 Chromium，设置移动视口、DPR、触摸能力和移动 User-Agent，不依赖 Playwright。它不需要 Android 模拟器，但也不等于测试了原生 WebView 容器。设备状态 GET API 只用于独立核验，禁止通过安装写 API 绕过网页流程。

## 2. 必需组件

1. **真实 ESP32 设备**：运行当前待测启动器固件，网页服务来自该设备。
2. **CDP 浏览器 UI 自动化**：一个持续的移动视口浏览器会话，所有创建、安装和删除动作都由页面控件触发。
3. **分层测试对象**：`tests/realdevice/data-child/` 专用固件用于基础协议/分区分配回归；真实市场业务 DATA 验收目标为录音笔 Play 28（`recordings` FAT 分区）。Play 28 的容量缩容与录音 UI 自动化尚未完成，详见 [真实市场固件选择](assets/mobile-page-data-e2e-market-firmware.zh_CN.md)。在该链路完成前，fixture PASS 不等于市场业务 E2E PASS。
4. **硬件运行驱动**：通过真实的设备控制/串口观测链路启动指定槽位、等待测试子固件执行、触发其返回启动器，并收集设备侧证据。不能用模拟的 API 响应代替。驱动通过 `--runtime-driver` 提供，接口见下文。
5. **DATA 分区支持**：本轮 A/B 测试固件都必须声明并实际访问受支持的 DATA 分区；否则本次完整生命周期测试应失败，而不是跳过读写测试。

## 3. 测试阶段与验收条件

| 阶段 | 实际动作 | 通过条件 |
|---|---|---|
| M01 | 手机视口打开真实设备页面 | 页面模块挂载成功、关键控件存在、无横向溢出、无未处理 JS 错误 |
| M02 | 读取槽位/安装器基线 | 状态可读且安装器空闲；记录槽位、DATA 预留和剩余空间 |
| M03 | 搜索测试固件 A，选择槽位/名称并确认 | 全程通过网页 UI，设备确认流程正常 |
| M04 | 启动 A 并执行运行时测试 | 驱动确认 A 真正启动；A 写 DATA、读回并校验内容；复位后再次验证持久性；返回启动器 |
| M05 | 通过网页安装 B，再启动 B | B 完成相同运行时测试；A/B 槽位与 DATA 分区互不覆盖 |
| M06 | 通过空间管理 UI 删除 A | A 不再存在；B 仍有效，并重新启动 B 验证运行和 DATA 未受影响 |
| M07 | 验证删除后的真实设备状态 | 设备侧证据确认 A 已不可启动、A 的 DATA 分区/预留已释放；此验证不得主动执行删除 |
| M08 | 通过 UI 删除 B 并检查最终状态 | B 不可启动，B 的 DATA 分区/预留释放；槽位、DATA 和 free space 恢复基线 |
| M09 | 故障证据 | 报告、截图、页面源码、运行驱动原始证据和失败原因均保存并脱敏 |

槽位列表和 DATA 预留 API 是辅助证据，不是子固件运行成功的充分条件。驱动证据必须来自设备侧（例如串口测试标记、硬件复位/按键动作记录和子固件 DATA 自检结果）。

## 4. 硬件运行驱动接口

运行器调用外部可执行文件，不会自行假定设备具备未定义的远程按键 API：

```bash
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device \
  --url 'http://<device-ip>/' --token '<32-hex-session>' \
  --play-a <test-play-id-a> --play-b <test-play-id-b> \
  --runtime-driver /absolute/path/to/device-runtime-driver
```

每次运行调用：

```text
--action boot-test-and-return
--slot <slot-index> --play-id <play-id> --slot-name <unique-name>
--phase <phase> --timeout-ms <milliseconds> --evidence-file <path>
```

驱动必须控制真实设备完成：启动指定槽位 → 确认子固件启动标记 → 让测试子固件在其 DATA 分区写入唯一 nonce/payload → 读回并验证字节/校验和 → 触发设备复位并确认数据仍在 → 通过真实操作返回启动器 → 确认启动器恢复运行。测试不能靠普通网页 JS 自己模拟这些行为。

驱动应在 `--evidence-file` 输出 JSON。每次 `boot-test-and-return` 必须至少包含以下布尔字段且全为 `true`：

- `childBooted`
- `dataWriteOk`
- `dataReadOk`
- `dataChecksumOk`
- `dataPersistedAfterReboot`
- `returnedToLauncher`

还应提供 `dataLabel`、`serialEvidence`（设备侧原始证据的文件路径或摘要）及可追踪的阶段信息。删除 A 后，运行器以只验证模式调用：

```text
--action verify-deleted --slot-name <unique-name> --play-id <play-id>
--timeout-ms <milliseconds> --evidence-file <path>
```

其证据必须包含 `deletedSlotNotBootable: true` 和 `dataPartitionReleased: true`。驱动的 `verify-deleted` 操作只能观察、尝试非破坏性验证；不得替运行器删除槽位或改写分配记录。

**重要：仓库当前只定义并调用这个驱动契约，不包含适配具体串口/继电器/按键硬件的通用驱动实现。** 在提供实际硬件驱动和测试子固件 A/B 之前，不能把本套测试报告标记为完整 E2E PASS。若设备没有自动按键/复位硬件，必须先明确可用的真机控制手段，不能猜测接口。

## 5. 安全约束

- 必须显式传入 `--real-device` 和 `--runtime-driver`。
- A/B 使用专用测试玩法 ID，必须不同；测试名称每轮唯一。
- 安装、删除只通过网页 UI；运行器不调用安装 prepare/session/chunk/finalize/remove 等写 API。
- 只读 API 只用于槽位、DATA 分配、安装器状态的交叉核验。
- 失败清理只允许删除本轮生成的精确名称；不确定目标时停止，不做猜测性清理。
- token、设备 IP 和凭证不得进入报告；保留截图、页面源码、设备侧证据并进行脱敏。

## 6. 当前验证边界

静态契约测试和 JavaScript 语法检查只能证明测试工具结构满足约束。没有连接真机、没有实际硬件运行驱动、没有专用测试子固件并成功采集设备侧证据时，设备 E2E 必须报告为 NOT RUN/FAIL，不能以只读 API 检查通过代替。



CDP 端点示例：

```bash
chromium --remote-debugging-port=9222 --user-data-dir=/tmp/meta-pass-e2e
node tools/realdevice/mobile_page_e2e.mjs --real-device --cdp-url http://127.0.0.1:9222 --url http://<device-ip>/ --token <32-hex-session> --play-a <test-id-a> --play-b <test-id-b> --runtime-driver /path/to/device-runtime-driver
```

## 7. 一次运行覆盖矩阵与问题汇总

运行器按阶段记录结果；单个安装、运行时或删除用例失败后，会继续执行后续仍具备前置条件的独立用例，而不是在第一个错误处结束。只有页面无法打开、设备基线不可读、安装器不空闲或基线分配几何不合法时，才会停止所有写操作，以免在未知设备状态下继续破坏性测试。最终报告汇总所有已执行失败项、被依赖阻塞的用例、未覆盖项和清理结果。

| 手机内嵌页用例 | 对应旧 WebView / USB 服务页覆盖 | 本运行器实际检查 |
|---|---|---|
| M01 页面与移动端兼容性 | 旧版 document ready、标题/正文、交互控件；USB E2 页面加载 | DOM ready、正文/控件存在、搜索/管理控件可见、触摸能力、移动 UA、无横向溢出、多组视口断点、JS/console/网络失败 |
| M02 设备基线与协议 | 旧版 slots/status 快照、USB E1/E4 | 只读 slots/status、安装器空闲、动态槽位协议、分配池范围、偏移/大小对齐、APP/DATA 不重叠、free-space 计算、基线 DATA 保留 |
| M03 搜索与候选详情 | USB E5 获取玩法元数据与安装候选 | 通过内嵌页输入玩法 ID、点击搜索、等待详情和安装面板；校验有可安装槽位且确认控件可用 |
| M04 安装 A | USB E6/E7/E8 | 通过内嵌页完成安装；只读 API 独立确认 VALID 槽位；记录 DATA 预留与几何 |
| M04A 运行 A | 旧版设备运行阶段 | 硬件驱动模式验证子固件启动、DATA 写入/读回/校验和/复位持久性、返回启动器；人工模式仅记录人工启动/返回确认 |
| M05 安装 B / 共存 | USB R2 双槽共存与剩余空间变化 | 通过内嵌页安装 B，核验 A/B 同时有效、分配不重叠、free-space 一致 |
| M05B 运行 B | 旧版设备运行阶段 | 与 A 相同的运行时证据要求 |
| M06 删除 A / 保留 B | USB R3 选择性删除与隔离 | 通过管理 UI 删除 A，确认 B 仍有效；再次运行 B；检查 A 的 DATA 预留释放 |
| M07 删除 B | USB R4 删除第二槽 | 通过管理 UI 删除 B；硬件驱动模式额外验证已删除固件不可启动、DATA 分区已释放 |
| M08 恢复基线 | USB E11/R4 基线恢复 | 最终槽位、DATA 预留、free-space 与起始状态对比 |
| M09 问题汇总 | USB runner 的失败日志与证据归档 | 汇总页面异常、console errors、失败请求；保存 JSON/文本报告、截图、HTML、基线/中间/最终设备快照及运行时证据 |

**执行语义：**FAIL 用于任何已执行失败项或因前置失败而被阻塞的必要用例；人工辅助模式最多为 PASS_WITH_MANUAL_STEPS；只有硬件驱动与专用测试子固件提供完整设备侧证据，才可能得到完整 PASS。USB Web Serial 的桌面 Chrome、DTR/RTS 硬复位专属步骤仍由 usb_web_e2e_multi.mjs 覆盖；手机内嵌页测试不会伪称自己验证了桌面 Web Serial 权限或 USB 线路电气行为。

### 一次性执行

在仓库根目录运行以下命令。该模式会尝试完成 A/B 生命周期并在同一份报告里汇集问题；建议使用专用测试玩法 ID，并保留自动生成的报告目录：

~~~bash
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device --manual-assist \
  --cdp-url http://127.0.0.1:9222 \
  --url 'http://<device-ip>/' --token '<32-hex-session>' \
  --play-a <test-play-id-a> --play-b <test-play-id-b>
~~~

若已配置真实硬件运行驱动，将 --manual-assist 替换为 --runtime-driver /absolute/path/to/device-runtime-driver。运行结束查看终端打印的 REPORT_DIR，其中 report.json 与 report.txt 是总结果；baseline-*.json、after-install-*.json、final-*.json、截图、页面源码和运行时证据用于定位各项问题。测试会尽力清理本轮唯一命名的 A/B 槽位，不触碰其他槽位。

## 8. 覆盖分级与人工辅助模式

### 覆盖边界

| 场景 | CDP/网页自动化 | 人工辅助模式 | 完整硬件驱动模式 |
|---|---|---|---|
| 页面响应式布局、JS/网络错误 | 自动 | 自动 | 自动 |
| 通过网页安装 A/B、删除 A/B | 自动 | 自动 | 自动 |
| 槽位状态、分配范围/对齐/重叠、剩余空间 | 只读交叉核验 | 只读交叉核验 | 只读交叉核验 |
| 实体 UP/DOWN/OK 启动玩法、返回启动器 | 不覆盖 | 人工操作并确认 | 由实际硬件驱动操作并采集设备证据 |
| 子固件 DATA 写入/读回/校验和/重启持久性 | 不覆盖 | **不作为已验证项**；除非另有可审计的设备侧测试协议 | 必须由测试固件和驱动提供证据 |
| 删除后固件确实不可启动 | 不覆盖 | **不覆盖**；槽位列表中不存在不能证明闪存中的镜像不可启动 | 必须有设备侧非破坏性验证证据 |
| DATA 预留释放与基线恢复 | 自动检查分配记录 | 自动检查分配记录，但不能独立证明物理 DATA 内容不可访问 | 分配记录 + 设备侧证据 |

### 人工辅助运行

没有专用硬件运行驱动时，可使用人工辅助模式执行可自动化的网页生命周期，并在实体按键步骤暂停：

```bash
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device --manual-assist \
  --url 'http://<device-ip>/' --token '<32-hex-session>' \
  --play-a <test-play-id-a> --play-b <test-play-id-b>
```

此模式要求交互式终端；非 TTY 环境会拒绝运行，避免无人值守时误把人工步骤当成通过。暂停时按终端说明操作：

1. 在设备上用实体 UP/DOWN 选择本轮新安装的测试玩法，再按 OK 启动。
2. 观察设备确实进入玩法，而非仍停留在启动器。仅看到标题不代表 DATA 测试通过。
3. 使用设备实际支持的退出/返回操作回到启动器。
4. 等待设备 USB 页面恢复后，在终端输入 `PASS`；无法启动、无法返回、画面不确定或设备无响应时输入 `FAIL`。
5. 对 A、B 以及删除 A 后再次启动 B，分别执行上述步骤。

人工输入只记录为 `manual-confirmed`，不是串口/固件自动采集的设备证据。此模式的最终状态是 `PASS_WITH_MANUAL_STEPS`，**不等价于完整 E2E PASS**。报告会明确列出未覆盖项：DATA 写入/读回/校验和/重启持久性，以及删除后镜像不可启动。若要验收这些项目，必须接入真实运行驱动和专用测试固件；不得通过人工点选或 API 列表结果伪造证据。

若提供 `--runtime-driver`，则使用硬件驱动模式；不可同时传入 `--manual-assist`。完整硬件模式的协议和证据要求见第 4 节。
