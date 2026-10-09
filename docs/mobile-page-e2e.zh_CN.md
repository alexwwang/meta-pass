<p align="right">
  <strong>简体中文</strong> · <a href="mobile-page-e2e.md">English</a>
</p>

# 手机端设备内嵌网页与子固件生命周期 E2E 测试

目标分支：`feat/storage`。

## 1. 测试目标

这不是只检查网页是否显示、槽位 API 是否返回预期值的测试。测试必须实际通过网页完成安装/删除，并在真机上启动安装的子固件、访问其 DATA 分区，验证数据写入、读回、校验和重启后的持久性。

网页操作使用 Playwright 的移动视口（当前实现）；它不需要 Android 模拟器，但也不等于测试了原生 WebView 容器。设备状态 GET API 只用于独立核验，禁止通过安装写 API 绕过网页流程。

## 2. 必需组件

1. **真实 ESP32 设备**：运行当前待测启动器固件，网页服务来自该设备。
2. **CDP CDP 浏览器 UI 自动化**：一个持续的移动视口浏览器会话，所有创建、安装和删除动作都由页面控件触发。
3. **专用测试子固件 A/B**：必须是可在该硬件上启动的测试镜像，且实现确定性的 DATA 分区读写协议。普通商店固件不能保证包含测试标记或 DATA 行为。
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
