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

基线契约（`tools/verify_firmware.py` 强制）逐字节保留：`factory@0x10000/3MB`、
`cardid@0x356000/0x4000`。新增分区只使用空隙与 cardid 之后的空闲区：

| 分区 | 类型 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 0x6000 | 不变（子固件共享此 NVS 命名空间） |
| phy_init | data/phy | 0xf000 | 0x1000 | 不变 |
| factory | app/factory | 0x10000 | 0x300000 | **不变**，meta-pass 本体 |
| otadata | data/ota | 0x310000 | 0x2000 | 新增；全 0xFF = 引导 factory |
| cardid | data/nvs | 0x356000 | 0x4000 | **不变**，受保护身份区 |
| ota_0 | app/ota_0 | 0x360000 | 0x200000 | 新增槽位 0（app 分区需 64KB 对齐，0x35A000 未对齐故从 0x360000 起） |
| ota_1 | app/ota_1 | 0x560000 | 0x200000 | 新增槽位 1；尾部余 0x760000–0x800000（640KB）备用 |

约束：子固件单镜像 ≤ 2044KB（槽位尾部最后 4KB sector 保留给显示名 blob，见 §6.2）；
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

1. 包含 `main/metapass_hook.h`，在处理 `BSP_BTN_LONG2`（OK 键 2× 长按）时调用
   `metapass_return_to_launcher()`（设置启动分区为 factory 并重启）。
2. 自检通过后调用 `esp_ota_mark_app_valid_cancel_rollback()` 以常驻。
3. 继续使用共享 NVS 时自行加命名空间前缀，避免与其他固件冲突。

按键模型（硬件裁决）：三键共用 GPIO0 单 ADC 分压节点，组合键会坍缩为优势单键
（UP+任何=UP；DOWN+OK≈212mV 落在 DOWN 窗口），**组合键不可用**；电源键是硬件电源控制，
固件不可读。因此返回机制采用**两级长按**：`LONG`（默认时长）= 应用内返回，`LONG2`
（2× 默认时长）= 退回启动器。BSP 扩展 `BSP_BTN_LONG2` 事件，向后兼容（`LONG` 语义不变）。

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
3. 页面用 esptool-js 经 USB Serial/JTAG 把子固件写入槽位偏移（`0x360000`/`0x560000`），
   写后自动校验；复位后 meta-pass 扫描即可引导。

固件来源二选一：

- **本地 `.bin`**：应用单镜像直接写；Full Flash 合并镜像则页面在 JS 里解包
  （读 `0x8000` 分区表定位 factory 应用，按 ESP 镜像格式走 segment 表算精确长度）。
- **社区玩法链接**：社区 API 无 CORS 头，由本地服务器（`server.mjs`）代理转发
  （同 passport-sim 的 community-import 模式，限制只代理 `ai-passport.folotoy.cn`）。
  社区详情接口提供 `firmwareSha256`，页面下载后校验哈希——与 meta-pass 启动扫描时
  显示的哈希形成闭环比对。

边界：只写两个槽位偏移，不触碰 factory/cardid/otadata；应用镜像 > 2MB 拒绝写入。
未覆盖：BLE 通道（速率慢、需自建分块协议、入口需 HTTPS 托管，评估后放弃，见 §11）。

### 6.2 槽位显示名 blob

固件真名（如 "Pocket Walkie"）只存在于商店元数据，镜像内的 `project_name` 普遍是编译模板
默认值（社区固件全是 `FoloToy-AI-Passport`），扫描时无法得知真名。故在**安装时**把显示名
写入槽位分区尾部最后 4KB sector（`slot_offset + 0x1FF000`）：

- blob 格式：`magic "MNAM"`(4B) + `name_len`(1B，1–32，对齐槽位注册表字段) + 名字（可打印 ASCII) + XOR 校验(1B);
- 启动器扫描：blob 校验通过 → 显示真名；否则回退 `project_name` 剥 `FoloToy-` 前缀的核心名；
- 名字来源：USB 安装页 = 社区玩法英文标题 / 本地文件名；Wi-Fi 导入页 = 可选输入框；
- 应用镜像上限随之收紧为 2044KB；删除槽位整区擦除，blob 一并消失。

## 7. 固件校验策略

强制（任何子固件）：

- 镜像头 magic `0xE9`、chip id = ESP32-C3、大小 ≤ 2044KB（槽位尾部保留显示名 blob）、segment 数量合法；
- 计算全镜像 SHA-256 并在确认页显示（供与来源方公布的哈希人工比对）。

可选徽章：固件包附带签名时验签显示「已签名」；不强制——市场上已有固件不能要求重新适配。
未签名固件启动前显示警告页，须 **LONG2** 确认。

诚实边界：未签名子固件一旦启动即拥有完整 Flash 权限，软件层面无法阻止恶意固件擦除
cardid。信任来源 = 用户判断 + 配对码物理持有 + 试运行隔离。eFuse 写保护/Secure Boot v2
（不可逆）留待未来单独评估，本期不做。

## 8. 本地管理界面

保留 `ui_pixel` 主题（天空/草地/标题牌/吉祥物）与右上角电量（避开白云 `x≈188,y≈8`）。
UI 文案英文。

- **主列表页**：槽位 0/1 条目显示 空 / 显示名（安装时写入的真名，无则回退核心名）；UP/DOWN 选择，OK 单击进详情。
- **详情页**：Boot（未签名须警告页 LONG2 确认）、Delete（确认页 LONG2 确认）、返回。
- **Import 页**：显示 SSID/密码/配对码/IP/倒计时；OK 长按退出并完整释放网络栈。
- 全局：`OK LONG` = 返回上级；`OK LONG2` 在子固件中 = 退回启动器（启动器内同 LONG）。

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
- 分区表：factory/cardid 与基线逐字节一致，otadata/ota_0/ota_1 无重叠、cardid 全 0xFF；
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
