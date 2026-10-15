<p align="right">
  <a href="mobile-page-data-e2e-test-firmware.md">English</a> · <strong>简体中文</strong>
</p>

# 手机内嵌页面 DATA E2E 专用子固件

状态：基础设施 fixture；不是市场业务 DATA 验收固件。真实市场目标与容量门槛见 [真实市场固件选择](mobile-page-data-e2e-market-firmware.zh_CN.md)。

## 目标

验证真实子固件 DATA 访问链路，而不是把安装器 HTTP 成功响应当作证据。测试镜像是一个独立 ESP-IDF 5.5.3 应用，通过 `esp_partition_find_first()` 按标签查找数据分区，并通过 `esp_partition_*` API 读写；不接受任意 Flash 偏移。

范围说明：这是可重复的存储契约测试固件，不是对某一个市场玩法业务逻辑的复制。它复现市场子固件使用声明式 DATA 分区、按标签解析、通过 ESP-IDF 分区 API 持久化应用状态的公共路径，并叠加仅测试构建存在的串口观测层。市场玩法源码未包含在当前仓库；若要覆盖某个玩法的具体业务状态机，应在该玩法原有存储代码上加入同样的测试观测层，而不是把此 fixture 的业务行为当成该玩法的证明。

同一镜像可以用不同 play ID 安装为 A、B。启动器会按当前 play ID 物化 DATA 分区，因此相同标签可以映射到不同物理区域。测试必须使用不同 nonce，并记录每个玩法报告的 DATA offset/size，证明隔离性。

## 文件布局

- `tests/realdevice/data-child/CMakeLists.txt`：独立 ESP-IDF 工程。
- `tests/realdevice/data-child/partitions.csv`：模板镜像声明 64 KiB 的 `e2edata` DATA 分区；安装时应在动态池内为其预留空间。
- `tests/realdevice/data-child/main/test_main.c`：USB Serial/JTAG 行协议和 DATA 操作。
- `tools/realdevice/data_child_serial.py`：主机侧串口协议客户端与证据采集。
- 本文档的英文版：`mobile-page-data-e2e-test-firmware.md`。

## 设备协议

USB Serial/JTAG 使用逐行 ASCII 命令，每个响应为一个 JSON 对象。命令范围刻意受限，不接受 Flash 偏移。

| 命令 | 设备端行为 |
|---|---|
| `HELLO` | 返回协议/构建信息和运行中的应用身份 |
| `INFO` | 按标签查找 `e2edata`，报告分区地址/大小 |
| `WRITE <32位十六进制nonce>` | 擦除 DATA 首个扇区，并通过已解析的分区句柄写入带版本的确定性记录 |
| `READ` | 从 DATA 读回记录，返回 nonce、序号和 SHA-256 |
| `ERASE` | 仅擦除测试记录所在的首个扇区 |
| `REBOOT` | 重启 ESP；预期启动器下次启动回到 factory |

该协议仅用于测试构建，禁止进入生产固件。只有经真实启动器/bootloader 生命周期后，由子固件重新读取的串口输出才是持久性证据。

## 必须完成的 E2E 顺序

1. 读取并保存启动器基线槽位/DATA 清单。
2. 通过真实手机内嵌页面安装 A、B。安装 manifest 必须声明 `e2edata` 及大小；设备端应按 play ID 分别分配 DATA 记录。
3. 通过真实启动器/bootloader 路径启动 A。记录 `INFO`，先发送 `ERASE` 并确认空记录读取返回 `DATA_INVALID`，再发送 `WRITE <nonce-A>` 和 `READ`；必须精确匹配 nonce、SHA-256 和 CRC32。
4. 返回启动器并启动 B，使用不同 nonce 重复。确认 B 的 DATA offset 与 A 不同且数据相互独立。
5. 返回启动器，重启/重新启动 A，要求 nonce-A 仍然存在；B 同理。主机收到 ACK 或启动器清单都不能证明持久性。
6. 通过页面删除 A。验证 A 的槽位已不存在/不可启动，DATA 记录按当前卸载策略释放或归档；再启动 B 并确认 nonce-B 仍可读取。
7. 删除 B，验证最终槽位/DATA 几何恢复到基线。

## 当前集成边界

专用子固件能在运行后测试 DATA 访问，但它本身不能让启动器选择某个槽位。现有 `mobile_page_e2e.mjs` 的 runtime-driver 契约要求 `boot-test-and-return` 和 `verify-deleted`；单独的串口客户端无法安全完成这些动作，因为当前分支已新增 `CONFIG_META_E2E_TEST_CONTROL` 门控的启动器 USB 控制通道及 `mobile_page_runtime_driver.py`：仅接受当前 carve registry 中已存在且经扫描验证可启动的槽位下标，复用 `meta_store_boot_slot()`，并只读返回槽位/DATA 清单；删除后还可通过旧 play ID 请求验证启动器确实拒绝启动。默认 `n`，生产构建必须保持关闭。它们尚未在 ESP-IDF 环境编译，也未在真机验证，因此完整硬件 E2E 仍未验收。

测试启动器构建方式：

```bash
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.e2e-test.defaults' reconfigure
idf.py build
```

该 overlay 仅开启测试控制通道，不可用于发布。真机验收仍需确认子固件重启后确实回到 launcher，以及 USB Serial/JTAG 的控制台输入在目标 IDF 配置下可用。

## 构建

在 ESP-IDF 5.5.3 环境中从本目录执行：

```bash
idf.py set-target esp32c3
idf.py build
```

生成的 `build/data_child.bin` 是普通子固件 app 镜像，走现有 meta-pass 安装/提取流程，不是全 Flash 镜像。需确认 marketplace/analyze manifest 声明 `e2edata`；app 二进制本身不携带 DATA 内容。

## 接入手机页面 E2E runner

先在测试启动器上启用上面的 test overlay，并用仓库受保护布局的既有流程部署；不要把测试配置用于发布，也不要用全盘烧录覆盖设备身份分区。把同一测试子固件作为两个不同 play ID 的测试条目发布/提供，且两个条目的 analyze manifest 都必须包含 `e2edata`。

```bash
python3 -m pip install pyserial
export META_PASS_E2E_SERIAL_PORT=/dev/cu.usbmodemXXXX  # 按本机实际端口填写
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device \
  --cdp-url http://127.0.0.1:9222 \
  --url 'http://<device-ip>/' \
  --token '<32-hex-session>' \
  --runtime-driver tools/realdevice/mobile_page_runtime_driver.py \
  --require-data-reservation \
  --play-a <test-play-id-a> --play-b <test-play-id-b>
```

该命令会在真实手机页面安装/删除玩法，并通过 USB 串口驱动启动子固件、写入/读回、复位后重新启动并验证 DATA。只有设备端控制通道、子固件协议、页面操作和最终基线恢复全部通过，才可能得到完整 E2E PASS。当前仍需在实际 ESP-IDF 5.5.3 环境构建并在板卡上验证此链路。

## 证据与限制

- 固件响应返回真实分区 offset/size，以及子固件读回的字节证据。
- 主机应对读回记录独立计算 SHA-256。
- 单独执行 `REBOOT` 不构成数据持久性证据；必须重新启动子固件并执行 `READ` 验证。
- 不得用 `esptool write-flash` 模拟子固件 DATA 访问，那会绕过生产数据分区路径。
- 人工辅助检查不能代替自动化 verdict；该模式必须保持 `PASS_WITH_MANUAL_STEPS`。
