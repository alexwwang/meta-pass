# meta-pass 设计文档

[English](meta-pass-design.md) | 简体中文

> 单一权威来源。修改 meta-pass 代码前必读；设计变更先改本文档并记录到「决策记录」。

## 1. 目标与定位

meta-pass 是 AI Passport 的**多固件启动器**：作为 factory 应用常驻设备，可以把其他适配本硬件的
固件导入本地 Flash 槽位并引导启动，无需每次重新烧录整片 Flash；同时提供本地管理界面，可查看、
启动、删除已存储的固件。

非目标：不替代 OTA 云服务；不做固件签名强制（见 §7）；不修改 bootloader（第一期）。

## 2. 术语

- **启动器（launcher）**：meta-pass 本体，烧录在 factory 分区。
- **子固件（child firmware）**：写入 ota 槽位的第三方/衍生应用固件（应用单镜像 `.bin`）。
- **已适配子固件**：包含 meta-pass 适配 hook（见 §5）的子固件。
- **试运行（trial boot）**：未适配子固件的启动语义——任何重启后自动回到启动器。

## 3. Flash 布局

3-Slot 架构（feat/shrink）：factory 缩至 1.44MB，cardid 前空隙复用为 ota_0，otadata 移至 Flash 尾部。
受保护的 `cardid@0x356000/0x4000` 不变，由 `tools/verify_firmware.py` 强制：

| 分区 | 类型 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 0x6000 | 不变（子固件共享此 NVS 命名空间） |
| phy_init | data/phy | 0xf000 | 0x1000 | 不变 |
| factory | app/factory | 0x10000 | **0x170000** | 从 3MB 缩至 1.44MB；meta-pass 启动器，目标 < 1.43MB |
| ota_0 | app/ota_0 | **0x180000** | **0x1D6000** (1.84MB) | 新增；复用 cardid 前空隙 |
| cardid | data/nvs | 0x356000 | 0x4000 | **不变**，受保护身份区 |
| ota_1 | app/ota_1 | 0x360000 | 0x200000 (2MB) | 不变（app 分区需 64KB 对齐，0x35A000 未对齐故从 0x360000 起） |
| ota_2 | app/ota_2 | **0x560000** | **0x29E000** (2.61MB) | 新增；子固件槽位或录音存储双用（见 §6.3） |
| otadata | data/ota | **0x7FE000** | 0x2000 | 从 0x310000 移至 Flash 尾部；全 0xFF = 引导 factory |

约束：子固件单镜像 ≤ (分区大小 − 4KB)（槽位尾部最后 4KB sector 保留给显示名 blob，见 §6.2）；
合并镜像中 cardid 区域必须全 0xFF；项目名保持
`FoloToy-AI-Passport`（门禁硬编码镜像文件名）。

## 4. 启动与回滚模型

启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`：

- **启动子固件**：启动器校验通过后 `esp_ota_set_boot_partition(ota_x)` + `esp_restart()`。
- **已适配子固件**：自检后调用 `esp_ota_mark_app_valid_cancel_rollback()` → 跨重启常驻。
- **未适配子固件**：从不自验证 → 任何重启（崩溃/断电/看门狗）后 bootloader 自动回退
  factory。即「试运行一次，重启即回启动器」。防变砖不需要第三方配合。
- **启动器自身**：`app_main` 早期调用 `esp_ota_mark_app_valid_cancel_rollback()` 标记
  factory 有效，保证回滚目标永远可用。

## 5. 子固件适配约定（可选但推荐）

子固件是独立编译的本仓库衍生物，适配以获得完整体验：

1. 包含 `main/metapass_hook.h`，在处理 `BSP_BTN_LONG`（OK 键长按 1.5 秒）时调用
   `metapass_return_to_launcher()`（设置启动分区为 factory 并重启）。
2. 自检通过后调用 `esp_ota_mark_app_valid_cancel_rollback()` 以常驻。
3. 继续使用共享 NVS 时自行加命名空间前缀，避免与其他固件冲突。

按键模型：OK 键只有一个长按阈值（约 1.5 秒）。在 meta-pass 内：`LONG` = 返回上级
页面；删除确认页 `LONG` = 确认危险操作；未签名启动警告页是 BOOT / CANCEL 菜单
（UP/DOWN 选择，OK 短按确认）。电源键是硬件电源控制，固件不可读；切换子固件靠
断电重启（按电源键关机，再开机）。

## 6. 导入通道与协议

Wi-Fi **SoftAP（AP-only）** + 本地网页上传。信任锚点 = 物理持有（看得见屏幕）：

1. 用户在启动器进入 Import 页 → 设备开启 WPA2 SoftAP，SSID `metapass-XXXX`，
   随机密码与 **6 位一次性配对码**显示在屏幕上；会话 N 分钟（默认 5）无活动自动关闭。
2. 上传方连接热点，浏览器打开 `http://192.168.4.1/`，输入配对码，选择槽位与 `.bin` 上传。
3. HTTP 约束（依 phoenixzhc 的 SoftAP 经验）：Content-Length 超上限立即拒绝；1024 字节
   分块流式写入（`esp_ota_begin/write` 直接写槽位，不整包入 RAM）；接收循环处理超时/断开；
   任何失败路径擦除槽位标记为无效。`max_connection=1`。
4. 写入完成后校验（§7），通过则标记槽位可启动，失败则擦除槽位。

HTTP API（最小集）：`GET /`（页面）、`POST /api/session`（配对码换会话）、
`POST /api/upload?slot=N`（body=固件，需会话）、`GET /api/status`。

资源预算：进入 Import 页前记录空闲堆与最大连续块；音频等大资源保持未初始化状态；
Wi-Fi/HTTP 在离开页面时完整停止并释放（对照 demo_wifi 的进入/退出模式）。

### 6.1 USB 串口安装通道（路线 A，与 Wi-Fi 导入并存）

免线缆之外的第二通道：**数据线 + Chrome 浏览器**。固件零改动，利用 ROM bootloader：

1. 设备按住 **UP 键**开机/复位：UP = 0Ω 拉低 GPIO0（strapping 键）→ 进入 ROM 下载模式。
2. 电脑 Chrome 打开 `tools/install-slot/` 页面（localhost 服务；Web Serial 要求安全上下文，
   设备端 `http://192.168.4.1` 无法满足，故页面只能在电脑端）。
3. 页面用 esptool-js 经 USB Serial/JTAG 把子固件写入槽位偏移（`0x180000`/`0x360000`/`0x560000`），
   写后自动校验；复位后 meta-pass 扫描即可引导。

固件来源二选一：

- **本地 `.bin`**：应用单镜像直接写；Full Flash 合并镜像则页面在 JS 里解包
  （读 `0x8000` 分区表定位 factory 应用，按 ESP 镜像格式走 segment 表算精确长度）。
- **社区玩法链接**：社区 API 无 CORS 头，由本地服务器（`server.mjs`）代理转发
  （同 passport-sim 的 community-import 模式，限制只代理 `ai-passport.folotoy.cn`）。
  社区详情接口提供 `firmwareSha256`，页面下载后校验哈希——与 meta-pass 启动扫描时
  显示的哈希形成闭环比对。

边界：只写三个槽位偏移，不触碰 factory/cardid/otadata；应用镜像 > (槽位大小 − 4KB) 拒绝写入。
未覆盖：BLE 通道（速率慢、需自建分块协议、入口需 HTTPS 托管，评估后放弃，见 §11）。

### 6.2 槽位显示名 blob

固件真名（如 "Pocket Walkie"）只存在于商店元数据，镜像内的 `project_name` 普遍是编译模板
默认值（社区固件全是 `FoloToy-AI-Passport`），扫描时无法得知真名。故在**安装时**把显示名
写入槽位分区尾部最后 4KB sector。因各槽位分区大小不同（ota_0=0x1D6000, ota_1=0x200000,
ota_2=0x29E000），blob 偏移按分区大小动态计算（`partition_size − 0x1000`）：

- blob 格式：`magic "MNAM"`(4B) + `name_len`(1B，1–32，对齐槽位注册表字段) + 名字（可打印 ASCII) + XOR 校验(1B);
- 启动器扫描：blob 校验通过 → 显示真名；否则回退 `project_name` 剥 `FoloToy-` 前缀的核心名；
- 名字来源：USB 安装页 = 社区玩法英文标题 / 本地文件名；Wi-Fi 导入页 = 可选输入框；
- 应用镜像上限随之收紧为 (分区大小 − 4KB)；删除槽位整区擦除，blob 一并消失。

### 6.3 ota_2 双用存储

ota_2 根据运行时状态承担两种角色：

- **子固件槽位**：第三方固件镜像可写入 ota_2 并由启动器通过 `esp_ota_set_boot_partition()` 引导。
- **录音存储**：录音子固件（运行于 ota_0 或 ota_1）可在 ota_2 上挂载 littlefs 存储音频文件。
  子固件先对 ota_2 执行 `esp_image_verify()`：发现合法镜像则不触碰；为空/无效则擦除并挂载为
  文件系统。

分区类型仍为 `app`（subtype `ota_2`），bootloader 可正常选择为启动目标。littlefs 挂载忽略
分区类型——`esp_littlefs_mount()` 按 label 定位分区，不关心 kind。此双用为运行时约定，
非分区表强制执行。

## 7. 固件校验策略

强制（任何子固件）：

- 镜像头 magic `0xE9`、chip id = ESP32-C3、大小 ≤ (槽位大小 − 4KB)（镜像之后紧跟单一
  4KB 尾部 metadata sector,携带 MSIG/MAEG/MNAM；各槽位大小不同：0x1D6000/0x200000/0x29E000）、segment 数量合法；
- 计算全镜像 SHA-256 并在确认页显示（供与来源方公布的哈希人工比对）。

签名徽章（应用层签名，可逆——不动 eFuse，不开 Secure Boot v2）：

子固件可在 `image_len` 之后附加 ECDSA-P256 签名徽章。签名使用编译期嵌入 meta-pass 的公钥
（`main/meta_sign_pubkey.h`，由 `tools/signing/public.pem` 生成）验签。OTA 分区内布局：

```
[app image (image_len 字节)] [尾部 metadata sector (4KB)]
                              ↑
  esp_image_verify 只校验 image_len 范围；尾部 sector 追加其后不影响。
  尾部 sector 布局:
    [0..127]       MSIG 预留区
    [128..4055]    MAEG 彩蛋窗口
    [4056..4095]   MNAM 显示名预留区
  MSIG 格式（可变长度，最大 81 字节）:
    [4B "MSIG"] [4B payload_len LE] [70..72B ECDSA-P256 DER 签名]
    [1B xor 校验（前 header+signature 字节异或）]
```

`scan_one` 在 `esp_image_verify()` 通过后调用 `meta_sign_verify()` 尝试验签。验签通过的槽位
在详情页显示「SIGNED」，按 OK 直接启动（跳过警告页）。无签名徽章的槽位（全 0xFF 或无 MSIG
magic）视为未签名——走未签名固件警告页的 BOOT / CANCEL 选择。

### 7.1 可选彩蛋元数据

尾部 metadata sector 中部的固定窗口可以携带可选 `MAEG` 文本。字段**定长 3928 字节**：文本不足
3919 字节时用 0xFF padding 填满，因此无论彩蛋多长，MNAM 窗口始终位于 sector 末尾 40
字节。它只是元数据：不进入签名 digest，不被 ECDSA 覆盖，也不改变信任语义；设备侧遇到
缺失或非法彩蛋数据应按不存在处理。

```
尾部 metadata sector offset 128（定长 3928 字节字段）:
  [4B "MAEG"] [4B payload_len LE] [3919B 文本区: 可打印 ASCII + 0xFF padding]
  [1B xor 校验,固定位于窗口内偏移 3927,覆盖前 3927 字节(含 padding)]
```

设备端查看:在对应槽位的详情页快速连按 `UP UP DOWN DOWN`(四个 CLICK,相邻两键间隔 <0.5 秒)
进入彩蛋页;`UP/DOWN` 滚动文本,`OK` 短按返回。无有效 MAEG 字段的槽位显示 "No egg.";
字段损坏显示 "Egg data corrupted."。

子固件签名：`tools/signing/sign-firmware.sh <app.bin> [--egg-text "..."]` 追加尾部
metadata sector。ECDSA-P256 私钥托管在 macOS Keychain（标签 `com.folotoy.meta-pass.signing`，
由 `tools/signing/bin/keychain-keygen` 生成），脚本经 `bin/keychain-sign` 签名，
私钥永不落盘成文件。公钥发布在 `tools/signing/public.pem`，并由
`tools/signing/gen-pubkey.py` 注入 `main/meta_sign_pubkey.h` 与 `main/metapass_hook.h`。
诚实边界：未签名子固件一旦启动即拥有完整 Flash 权限，软件层面无法阻止恶意固件擦除
cardid。签名徽章证明固件来源（由 meta-pass 密钥持有者签名），但不在硬件层面强制阻断启动。
信任来源 = 用户判断 + 配对码物理持有 + 试运行隔离。eFuse 写保护/Secure Boot v2
（不可逆）留待未来单独评估，本期不做。

## 7.2 Launcher 升级契约(保数据)

分区布局已稳定;launcher 升级永远不得触碰用户数据。约束规则:

- **允许写入集**(原地升级仅可写这些区域):`bootloader@0x0`、
  `分区表@0x8000`、factory 应用 `@0x10000`,以及擦除态的
  `otadata@0x7FE000`(重启后回到 factory)。
- **永不写入**:`nvs@0x9000`(存储数据:Wi-Fi 配置、应用内部状态)、`cardid@0x356000`、
  `ota_0/1/2`(已装子固件)。它们在每次升级中原样保留。
- **布局门禁**:写入前,安装器读回设备分区表(`0x8000` 起 4KB)与升级包内
  `partition-table.bin` 逐字节比对。任何差异都拒绝升级——"只有 factory 会变"
  的前提不再成立。
- **产物门禁**:`tools/verify_firmware.py` 要求合并 8MB 镜像中的
  `nvs`/`ota_0`/`ota_1`/`ota_2`/`otadata` 保持全擦除态(0xFF)。完整镜像仅用于
  出厂烧录——esptool 对每个写入扇区都会先擦除,把它直接刷到已有设备上会
  摧毁用户数据。
- **工具链**:`tools/build-firmware.sh` 产出 `build/upgrade/`(四个文件 +
  `flash-args.txt`);USB 安装页提供「7. 升级 launcher」,实现读回门禁与最小写入集。

## 8. 本地管理界面

保留 `ui_pixel` 主题（天空/草地/标题牌/吉祥物）与右上角电量（避开白云 `x≈188,y≈8`）。
UI 文案英文。

- **主列表页**：槽位 0/1/2 条目显示 空 / 显示名（安装时写入的真名，无则回退核心名）；UP/DOWN 选择，OK 单击进详情。
- **详情页**：Boot（未签名走警告页）、Delete（确认页 OK LONG 确认）、返回。
- **未签名警告页**：警告文本 + BOOT / CANCEL 两行；UP/DOWN 选择，OK 短按确认；默认停在 CANCEL；OK 长按取消（回详情页）。
- **Import 页**：显示 SSID/密码/配对码/IP/倒计时；OK 长按退出并完整释放网络栈。
- 全局：`OK LONG` = 返回上级；子固件内 `OK LONG` = 退回启动器。

删除 = `esp_partition_erase_range` 整个槽位 + 清元数据；不影响 NVS 中子固件自存数据
（子固件命名空间自理）。

## 9. 测试策略（TDD）

与 ESP-IDF/LVGL 解耦的纯逻辑先行，host tests 覆盖：

- `meta_image`：镜像头/大小/chip id/segment 校验（合法、坏 magic、错芯片、超尺寸、截断）；
- `meta_slots`：槽位注册表与状态迁移（空/已占用/可启动/无效）；
- `meta_import`：导入状态机（idle→ap→paired→receiving→verifying→done|error）、配对码
  生成与比对、Content-Length 上限策略。

新增测试接入 `tools/validate.sh --static`。硬件相关路径（烧入、启动、回滚）列入真机验收。

## 10. 验收标准

- `./tools/validate.sh` 全绿（静态 + 固件门禁，含新 host tests）；
- 分区表：factory/cardid 与基线逐字节一致，ota_0/ota_1/ota_2/otadata 无重叠、cardid 全 0xFF；
- 真机清单（交付时逐项确认）：导入 1 个固件并启动；断电重启自动回启动器；已适配固件
  常驻；删除后槽位为空；坏文件被拒绝；配对码错误被拒绝；反复进出 Import 无泄漏。

### 10.1 模拟器端到端验证（2026-09-11，esp-emu 本地实例）

路线 A 的串口传输本身不可在模拟器验证（Web Serial 只枚举真实设备、模拟器不跑 mask ROM
下载模式），但「安装后状态」可 byte-for-byte 等效构造：用 esptool `merge_bin` 把社区固件
play 105（口袋对讲机）经 `tools/install-slot/extract-app-image.js` 解包后的应用镜像预置到
`ota_0@0x360000`（Full 镜像 SHA-256 与社区公布值一致），上传到模拟器后全链路通过：
启动器扫描识别槽位（size 1262KB、SHA-256 `bf98f879…` 与宿主侧计算一致）→ 详情页 →
未签名警告页 → LONG2 确认 → 子固件启动运行（WALKIE UI）→ 硬重启后按回滚模型自动回到
启动器（未适配固件 = 试运行）。未覆盖：串口传输、Wi-Fi 导入（模拟器无 AP 支持）。

2026-09-11 第二轮（双槽位 + 显示名 blob）：合成镜像预置 play 105@ota_0（"Pocket Walkie"
blob）与 play 81@ota_1（"Passport Radar" blob）。槽位列表正确显示真名；两槽均可启动；
Radar（官方固件，标准 BSP）菜单内按键导航正常——证明 meta-pass 引导路径不透传按键是
伪命题，按键经 ADC 注入正常工作。两个已知模拟器边界：Walkie（社区固件）独立整片烧录时
按键同样无响应（非 meta-pass 引入，疑其按键读取路径与模拟器 ADC 注入不兼容，真机待验）；
Radar 主功能依赖 BLE，模拟器检测到 BLE 即暂停（模拟器无 BLE 支持）。

### 10.2 三槽位瘦身验证（2026-09-12，Web 模拟器）

`feat/shrink` 从全新 `sdkconfig.defaults` 重编（不复用陈旧 `sdkconfig`）
**1,024,608 字节（1001 KB）**，对 1.44 MB（`0x170000`）factory 分区余量 32.0%。

决定性验证点是**显示名 blob 动态偏移**（`分区大小 − 4KB`）：旧的固定 `0x1FF000`
会把 ota_0 的 blob 放到 `0x37F000`，即落在 ota_1 分区内部。用合成 8MB 镜像预置三槽后
上传模拟器，逐槽位读回结果：

| 槽位 | 分区 | blob 偏移 | OCR 读回 |
| --- | --- | --- | --- |
| ota_0 | 0x180000 / 0x1D6000 | 0x355000 | `SLOT 0: Pocket Walkie` |
| ota_1 | 0x360000 / 0x200000 | 0x55F000 | `SLOT 1: Passport Radar` |
| ota_2 | 0x560000 / 0x29E000 | 0x7FD000 | `SLOT 2: Walkie Clone` |

三个名字各自从本槽位尾部读回，只有按本槽位分区大小计算偏移才可能成立。
`ota_0` 引导正常（`esp_ota_set_boot_partition` 指向新的 0x180000），断电后按回滚模型
回到启动器且槽位状态保持，回滚机制未受布局变更影响。

未覆盖：`ota_1`/`ota_2` 引导（与 ota_0 同代码路径，仅分区句柄不同）、删除流程、
ota_2 录音存储双用的另一半（尚无录音子固件）。

## 11. 决策记录

| 日期 | 决策 | 备选 | 理由 |
| --- | --- | --- | --- |
| 2026-09-10 | 从 main 开 feature/meta-pass | 直接在 main 开发 | 仓库约定：main 保持上游基线 |
| 2026-09-10 | 2 槽 × 2MB | 3 槽 × 1.5MB | 当前基线固件 1.48MB，1.5MB 无增长余量 |
| 2026-09-10 | SoftAP+网页上传 | USB 串口传输 | 免线缆；仓库有 SoftAP 资源预算经验 |
| 2026-09-10 | 屏幕一次性配对码 | 固定密码/双因子 | 物理持有即信任锚；操作简单 |
| 2026-09-10 | 完整性强制 + 签名可选 | 强制验签 / eFuse SBv2 | 不能要求市场存量固件重新适配；eFuse 不可逆 |
| 2026-09-10 | 回滚开启、未适配=试运行 | 要求子固件 mark_valid | 对第三方无约束力；崩溃自动回启动器天然防变砖 |
| 2026-09-10 | LONG2 两级长按返回 | 组合键 on+ok | ADC 单节点组合键物理不可区分（实测电压推导） |
| 2026-09-10 | 不改 bootloader | 自定义 bootloader 一键恢复 | GPIO0 是 strapping 键，ADC 上拉不支持开机按键恢复；风险大 |
| 2026-09-11 | 增加 USB 串口安装通道（路线 A） | 运行中自定义串口协议（路线 B） | 零固件改动；ROM bootloader + esptool 校验成熟；UP 键天然是下载模式触发器 |
| 2026-09-11 | 安装页放电脑端 localhost | 设备端伺服 / 公网托管 | Web Serial 需安全上下文；plays API 无 CORS 头需本地代理 |
| 2026-09-11 | 支持社区链接 + full 镜像 JS 解包 | 仅接受 app 单镜像 | 社区只发 full 镜像；解包为确定性算法；社区自带 firmwareSha256 可闭环 |
| 2026-09-11 | 放弃 BLE 导入通道 | BLE GATT 分块传输 | 速率慢（2MB 需数分钟）、需自建协议、入口须 HTTPS 托管、模拟器不可验证 |
| 2026-09-11 | 显示名存槽位尾部 4KB blob | NVS 存储；内置 play 名单 | USB 安装页在 ROM 下载模式只能写裸 flash，写不了 NVS 结构；内置名单随市场新增即过时 |
| 2026-09-11 | esptool-js 本地化 vendor | jsdelivr CDN 动态 import | CDN 慢/不可达时顶层 await 卡死整页（真机首测即踩）；本地 3 文件 81KB 零外链；同时修复 name-blob.js 未入静态白名单导致页面模块整体加载失败的 bug |
| 2026-09-12 | 3-Slot 架构（feat/shrink） | 保持 2 槽 × 2MB | factory 缩至 1.44MB；cardid 前空隙复用为 ota_0；ota_2 扩至 2.61MB 支持双用存储 |
| 2026-09-12 | blob 偏移按槽位分区大小动态计算 | 固定 0x1FF000 偏移 | 各槽位分区大小不同（0x1D6000/0x200000/0x29E000）；blob 偏移 = 分区大小 − 4KB |
| 2026-09-12 | -Os 编译器优化 + WARN 日志 | 保持 -Og Debug | -Os 缩小约 20%；INFO 级日志字符串占约 50KB .rodata |
| 2026-09-12 | 裁剪 LVGL examples/demos | 保持完整 LVGL | 默认构建编译 1800+ demo 单元（约 2MB）；启动器仅需 label/button/panel |
| 2026-09-12 | ota_2 双用：固件槽位或录音存储 | 独立存储分区 | 运行时 esp_image_verify() 判断；littlefs 忽略分区类型；无分区表冲突 |
| 2026-09-14 | 移除 LONG2，只留一个 LONG 阈值（1.5 秒） | 保留 LONG/LONG2 两级 | LONG 在多数页面是"返回"，在警告页却是"确认启动"——同样按住时长语义相反，用户会混淆 |
| 2026-09-14 | 未签名启动确认 = BOOT / CANCEL 菜单（UP/DOWN 选择，OK 短按确认，默认 CANCEL） | OK 长按确认 | 与主列表/详情页同一套"UP/DOWN 选择 + OK 短按确认"模型；LONG 全局保持"返回" |
