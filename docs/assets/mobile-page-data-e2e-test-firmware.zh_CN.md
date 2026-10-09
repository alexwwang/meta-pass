<p align="right">
  <a href="mobile-page-data-e2e-test-firmware.md">English</a> · <strong>简体中文</strong>
</p>

# 手机内嵌页面 DATA E2E 专用子固件

状态：已在 `feat/storage` 建立实现基线；真机集成尚未完成。

## 目标

验证真实子固件 DATA 访问链路，而不是把安装器 HTTP 成功响应当作证据。测试镜像是一个独立 ESP-IDF 5.5.3 应用，通过 `esp_partition_find_first()` 按标签查找数据分区，并通过 `esp_partition_*` API 读写；不接受任意 Flash 偏移。

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
3. 通过真实启动器/bootloader 路径启动 A。记录 `INFO`，发送 `WRITE <nonce-A>`，再发 `READ`；必须精确匹配 nonce 和 SHA-256。
4. 返回启动器并启动 B，使用不同 nonce 重复。确认 B 的 DATA offset 与 A 不同且数据相互独立。
5. 返回启动器，重启/重新启动 A，要求 nonce-A 仍然存在；B 同理。主机收到 ACK 或启动器清单都不能证明持久性。
6. 通过页面删除 A。验证 A 的槽位已不存在/不可启动，DATA 记录按当前卸载策略释放或归档；再启动 B 并确认 nonce-B 仍可读取。
7. 删除 B，验证最终槽位/DATA 几何恢复到基线。

## 当前集成边界

专用子固件能在运行后测试 DATA 访问，但它本身不能让启动器选择某个槽位。现有 `mobile_page_e2e.mjs` 的 runtime-driver 契约要求 `boot-test-and-return` 和 `verify-deleted`；单独的串口客户端无法安全完成这些动作，因为当前启动器没有经过认证的 test-only 槽位选择/重启命令，USB Serial/JTAG 也不能模拟 GPIO 按键。控制路径实现并经真机验证前，不得宣称完整硬件 E2E 已自动化。

下一步是在测试构建中增加显式门控的启动器控制通道：仅接受当前 carve registry 中已存在的槽位下标，验证槽位可启动后调用既有 `meta_store_boot_slot()` 并重启。不得接受裸偏移、任意分区标签或 Flash 写命令。生产构建必须将该通道编译掉。driver 还需区分预期的子固件重启窗口与串口超时。

## 构建

在 ESP-IDF 5.5.3 环境中从本目录执行：

```bash
idf.py set-target esp32c3
idf.py build
```

生成的 `build/data_child.bin` 是普通子固件 app 镜像，走现有 meta-pass 安装/提取流程，不是全 Flash 镜像。需确认 marketplace/analyze manifest 声明 `e2edata`；app 二进制本身不携带 DATA 内容。

## 证据与限制

- 固件响应返回真实分区 offset/size，以及子固件读回的字节证据。
- 主机应对读回记录独立计算 SHA-256。
- 单独执行 `REBOOT` 不构成数据持久性证据；必须重新启动子固件并执行 `READ` 验证。
- 不得用 `esptool write-flash` 模拟子固件 DATA 访问，那会绕过生产数据分区路径。
- 人工辅助检查不能代替自动化 verdict；该模式必须保持 `PASS_WITH_MANUAL_STEPS`。
