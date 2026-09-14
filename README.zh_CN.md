# meta-pass — 把 FoloToy AI Passport 变成多固件设备

[English](README.md) | 简体中文

meta-pass 是给 FoloToy AI Passport（ESP32-C3，8MB Flash）写的**多固件启动器**：
烧一次 meta-pass，之后就能随时往三个槽位里装社区固件、在菜单里点选即启动，
**不用再整片重刷**。想玩的玩法之间互相不覆盖，随时切回启动器。

<p align="center">
  <img src="docs/assets/images/meta-pass-cover.png"
       alt="meta-pass 启动器：三槽列表，显示 Pocket Walkie / Passport Radar / 空 Slot 2"
       width="800">
</p>

[\![FoloToy 玩法 #281](https://img.shields.io/badge/%E7%8E%A9%E6%8F%9C%E7%AD%94-281-informational)](https://ai-passport.folotoy.cn/plays/281)

官方机器一次只能跑一个固件，试社区的 plays 就得整片刷掉再刷回来。meta-pass
把自己放在 factory 分区当启动器，把剩余 Flash 划成三个 OTA 槽位装子固件：

```text
0x000000   bootloader
0x008000   分区表             nvs / phy_init（与官方一致）
0x010000   factory (1.44MB)  ← meta-pass 启动器本体
0x180000   ota_0 (1.84MB)    ← 槽位 0（尾部 4KB = 显示名 blob）
0x356000   cardid (16KB)     ← 设备身份，所有通道都不碰
0x360000   ota_1 (2MB)       ← 槽位 1（尾部 4KB = 显示名 blob）
0x560000   ota_2 (2.61MB)    ← 槽位 2：子固件槽或录音存储（双用）
                               双用途：检测到镜像时启动，无镜像则挂载 littlefs
0x7FE000   otadata           ← 启动器选槽后写这里再重启
```

选槽 = 写 otadata + 重启，由标准 2nd-stage bootloader 完成切换，无任何自定义
bootloader 改动。

## 功能

- **三槽位切换**：列表显示每个槽位的固件名/版本/大小/SHA-256，点选即启动；
  显示名在安装时写入（社区固件的 project_name 都是模板默认值，真名只能从安装通道带来）。
- **双用槽位 2**：为空时可当作 littlefs 存储（如录音固件）；启动器检测到槽内无
  合法镜像后自动挂载 FS，不再浪费 Flash。
- **两种安装通道**：
  - **USB 串口安装页**（推荐）：Chrome 打开本地页面，按住 UP 开机进 ROM 下载模式，
    直接写槽位；支持本地 `.bin`（Full 镜像自动解包）和 plays 市场链接
    （自动下载并按商店公布的 SHA-256 校验）；
  - **设备热点 + 网页导入**：设备开 SoftAP 显示配对码，手机/电脑连上后网页上传。
- **完整性校验**：magic、chip id、尺寸、segment 结构逐层校验，`esp_ota_end()` 权威
  复核；SHA-256 在详情页可见，可与商店公布值对照。
- **未签名警告**：未签名固件启动前需超长按 OK（LONG2）确认；恶意固件仍有完整 Flash
  读写能力（无 eFuse 强制签名），所以只装你信任来源的固件。
- **永不困在子固件里**：未适配的子固件一律按"试运行"处理——任何重启（含断电）都
  自动回启动器；适配过的固件可以长期驻留，并提供 LONG2 返回启动器。
- **身份区安全**：`cardid` 分区被所有安装/烧录路径避开；`verify_firmware.py` 在
  门禁里逐字节校验基线布局。

<p align="center">
  <img src="docs/assets/images/meta-pass-usb-installer.png"
       alt="USB 串口安装页：连接、选槽、选固件来源、显示名、进度与日志"
       width="800">
  &nbsp;&nbsp;&nbsp;
  <img src="docs/assets/images/meta-pass-wifi-import.png"
       alt="Wi-Fi 导入页：SSID、密码、一次性配对码、倒计时"
       width="800">
  &nbsp;&nbsp;&nbsp;
  <img src="docs/assets/images/meta-pass-unsigned-warning.png"
       alt="未签名固件警告：需超长按 OK 确认才能启动"
       width="800">
</p>

## 快速开始

### 1. 烧录 meta-pass（只做一次）

从 Releases 下载 `meta-pass_v0.2.2.bin`，或自行构建（见下文「开发」）。然后：

```bash
python -m esptool --chip esp32c3 -p <串口> -b 460800 \
    write-flash 0x0 meta-pass_v0.2.2.bin
```

只会烧到 `0x780000` 为止，不触碰 `cardid`（刷机工具默认只擦写覆盖区域；
**不要**用 `erase-flash` 整片擦除已写入身份的设备）。

### 2. 安装子固件

**方式 A：USB 串口安装页**（不需要设备开热点）：

用 Chrome 打开 **https://meta-pass.pages.dev/**（托管页面 + API 代理，零安装）——
或本地运行 `node tools/install-slot/server.mjs` → http://localhost:4191/。

设备按住 UP 键开机 → 页面 Connect → 选槽位 → 选本地文件或粘贴 plays 链接 →
Install → 断电重启。完整指南：[install-slot/README.zh_CN.md](install-slot/README.zh_CN.md)。

**方式 B：设备热点导入**（不需要电脑有 Chrome）：

主列表选 IMPORT FIRMWARE → 设备开热点并显示配对码 → 手机/电脑连上访问
`192.168.4.1` → 输入配对码、选槽位、上传 `.bin`。

### 3. 启动

主列表 UP/DOWN 选槽位，OK 进详情，BOOT 确认。未签名固件需 LONG2（超长按 OK）
确认。

## 按键操作

| 页面 | UP/DOWN | OK 单击 | OK LONG2（3 秒） |
| --- | --- | --- | --- |
| 主列表 | 选择槽位 | 进入详情 / 进导入页 | — |
| 槽位详情 | 选 BOOT/DELETE/BACK | 确认 | 删除需 LONG2 防误删 |
| 未签名警告 | — | 取消 | 确认启动 |
| 导入页 | — | — | 退出导入、回主列表 |

适配过的子固件内：OK LONG2 = 返回启动器（子固件自行挂接，见下节）。

## 子固件适配（可选）

子固件不改造也能跑（试运行模式）。想长期驻留 + LONG2 返回，包含
`main/metapass_hook.h` 并实现两条：

1. 自检通过后调用 `metapass_mark_valid()`（否则重启即回启动器）；
2. 把 OK 键的 LONG2 事件接到 `metapass_return_to_launcher()`。注意 LONG 事件
   在 1.5 秒先触发，给 LONG 分配一个无害动作（如返回上级页面）。

签名徽章（可选）:`tools/signing/sign-firmware.sh <app.bin> [private.pem]
[--egg-text "..."]` 在镜像后追加 ECDSA-P256 徽章（可附带彩蛋文本）;meta-pass
详情页显示 SIGNED 并跳过警告页直接启动。签名私钥由 meta-pass 发布方持有——
第三方开发者提交二进制给发布方签名，不能自签。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `main/` | 启动器 UI（`main.c`）、存储层（`meta_store`）、Wi-Fi 导入（`meta_net`）、纯逻辑模块（`meta_image`/`meta_slots`/`meta_import`/`meta_name`）、子固件 hook（`metapass_hook.h`） |
| `components/bsp/` | 板级支持包（官方原样 + `BSP_BTN_LONG2` 事件） |
| `install-slot/` | USB 串口安装页，线上地址 https://meta-pass.pages.dev/（Cloudflare Pages：静态资源 + `_worker.js` API 代理） |
| `tools/install-slot/` | 安装页本地开发副本（`server.mjs` 本地服务器，零依赖） |
| `tools/validate.sh` | 统一门禁：静态检查 + host tests + 固件构建 + 受保护布局校验 |
| `tests/` | host tests（C，纯逻辑模块，PC 上跑） |
| `docs/assets/meta-pass-design.zh_CN.md` | 设计文档（含决策日志与验收清单） |

## 开发

```bash
source <esp-idf-v5.5.3>/export.sh   # 必须 ESP-IDF v5.5.3
./tools/validate.sh --static        # 仓库检查 + host tests
./tools/validate.sh --firmware      # 固件构建 + 受保护布局校验（在 /tmp 隔离构建,
                                    # 产物拷回 build/meta-pass_v<版本>.bin）
node tools/install-slot/test-extract.mjs   # 安装页解包/名字 blob 测试
```

固件侧改动以 host tests 为先（TDD）；镜像解析/槽位元数据/校验和都是纯逻辑模块，
不依赖 ESP-IDF。

## 验证记录

| 类别 | 结果（2026-09-13） |
| --- | --- |
| 构建 | `validate.sh` 全门禁 PASS；应用 1,024,880 / 1,507,328 B（32% 余量）；合并镜像 8 MB；`cardid` 不动 |
| Host tests | `meta_image`/`meta_slots`/`meta_import`/`meta_name` 四套件全过；安装页 node 测试 7/7；**新增** `test_meta_net_contract.py` 钉住 JS↔C 路由契约（方法+路径一致性）；**新增** `test_meta_net_upload.c`（21 用例）用真实 FIPS 180-4 SHA-256 驱动完整的配对→上传→刷入→校验→blob 流程，ESP-IDF 桩替换，零硬件可测 |
| 模拟器（passport-sim） | 三槽列表（含动态 blob 偏移的真名：ota_0→0x355000，ota_1→0x55f000）；导航；详情元数据（`name: Pocket Walkie`，ver 1，1262 KB，sha 前缀）；空槽 BOOT 无操作；未签名警告页；LONG2 启动 ota_0；硬重启回滚到启动器；ota_1 Passport Radar 启动 + 回滚；DELETE→LONG2 擦除（SLOT 1→empty，重启后持久）；IMPORT 页（凭证/配对码/倒计时）；两项观察到判定为非固件 bug：确认页残留行 = 模拟器画布脏区伪影；导入页长按退出需更长按住 = 模拟器时序模型（300ms 注入延迟 + QEMU 时钟慢） |
| GitHub Actions | 静态检查（Linux/GCC）✅、固件门禁（ESP-IDF Docker）✅ |
| CI artifact SHA-256 | `b86ca4fe…1b28e773`（分发包权威参考；本地编译因嵌入时间戳哈希不同） |

## 与官方固件的关系

本仓库基于 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)（`f75873f`,
MIT）开发：`factory`/`cardid` 布局、`verify_firmware.py` 等基线契约逐字节保持兼容,
官方 demo 页被移除以容纳启动器 UI。非官方项目，与 FoloToy 无隶属关系。

## 常见问题

| 现象 | 处理 |
| --- | --- |
| 串口选择框是空的 | 设备没进下载模式（按住 UP 再开机），或 USB 线只能充电 |
| 子固件里按键没反应 / 无法 LONG2 返回 | 未适配固件没有返回钩子；断电重启即回启动器（回滚机制），这是设计行为 |
| 子固件重启后回到了启动器 | 未适配固件 = 试运行；想常驻需固件侧接 `metapass_mark_valid()` |
| 槽位显示 "AI-Passport" 而不是玩法名 | 该固件经合成镜像/旧通道装入，没有显示名 blob；用 USB 安装页重装并填 Display name |
| 镜像被拒绝 | 超过槽位上限（分区大小 − 4KB，尾部 4KB 保留给名字 blob），或不是 ESP32-C3 镜像 |
