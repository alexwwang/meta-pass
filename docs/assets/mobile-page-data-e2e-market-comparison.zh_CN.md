# 市场语音与电子书固件：DATA 存储机制对比

日期：2026-10-09
分支：`feat/storage`
目的：判断 DeepSeek 语音玩法与电子书阅读器是否能替代录音笔 Play 28，作为动态 DATA carve 的真实业务验收对象。

## 结论摘要

- **DeepSeek 语音 / FoloToy AI Passport：当前公开可核对的实现不等同于录音笔 DATA 玩法。** 小智 AI Passport 的公开配置目标是 ESP32-C3、8 MB Flash；v2 分区表有共享 NVS 和 2 MiB `assets` SPIFFS 子类型分区，没有单独的录音/用户档案 DATA 分区。源码用 NVS 保存设置，`assets` 通过分区标签查找后直接读取/内存映射。语音交互以实时音频流为主。它适合研究 NVS 与 assets，不适合作为验证独立、可按玩法隔离的 FAT 用户数据 carve 的首选对象。
- **电子书阅读器：开源候选实现有独立用户数据分区，但不能直接当作之前市场里的那款电子书玩法。** 本次找到的公开项目 CHENXiNNNX/Ebook 使用 `userdata` FAT（6 MiB）、`assets` LittleFS（4 MiB）及外部 TF 卡。用户文件可放在内部 `/int` 或外部 `/sd`；内部 FAT 通过标签 `userdata` 挂载。它能作为标签寻址与多存储介质的源码参考，但这不是已确认的市场固件二进制。
- **录音笔 Play 28 仍是当前最佳真实市场 DATA 验收目标。** 已有的市场二进制分析确认 `recordings` FAT 分区、运行时生成录音、WAV 导出和删除流程。缩容与真机 UI 自动化仍未完成。

## 证据分级

| 对象 | 本次可获得证据 | 可确认的内容 | 尚不能声称 |
|---|---|---|---|
| FoloToy AI Passport / DeepSeek 配置 | 公开源码、目标板配置、v2.5.0 release 元数据 | 分区表、NVS 设置 API、assets 查找与访问路径 | 尚未完成发布 ZIP 二进制的本地字节级校验；也未证明该版本就是设备上某个市场条目的精确固件 |
| 开源 Ebook 项目 | 公开源码、分区 CSV、用户数据说明 | userdata FAT / assets LittleFS / TF 卡的分工 | 不是已确认的目标市场电子书玩法固件 |
| 录音笔 Play 28 | 仓库既有市场固件字节级调研 | `recordings` FAT，按标签挂载，运行时录音数据可导出/删除 | 精确市场版本和自动化 UI 契约仍未钉住 |

## 1. DeepSeek 语音：FoloToy AI Passport / 小智 AI

### 目标与版本

公开上游：`78/xiaozhi-esp32`，release `v2.5.0`，发布资产中存在 `v2.5.0_folotoy-ai-passport.zip`（资产大小约 2.23 MB）。板卡配置 `main/boards/folotoy/ai-passport/config.json` 明确设置目标为 `esp32c3`、Flash 为 8 MB，并使用 `partitions/v2/8m.csv`。

- 上游仓库：https://github.com/78/xiaozhi-esp32
- AI Passport 板卡配置：https://github.com/78/xiaozhi-esp32/blob/main/main/boards/folotoy/ai-passport/config.json
- v2 8 MiB 分区表：https://github.com/78/xiaozhi-esp32/blob/main/partitions/v2/8m.csv
- v2.5.0 release：https://github.com/78/xiaozhi-esp32/releases/tag/v2.5.0

“DeepSeek 语音”在这里指设备运行语音固件、再配置 DeepSeek 作为模型提供方；不是一个叫 DeepSeek 的独立文件系统实现。

### 分区布局（v2 / 8 MiB）

| 标签 | 类型 / 子类型 | 容量 | 观察到的作用 |
|---|---|---:|---|
| `nvs` | data / nvs | 16 KiB | 设备设置等键值持久化 |
| `otadata` | data / ota | 8 KiB | OTA 启动状态 |
| `phy_init` | data / phy | 4 KiB | PHY 初始化 |
| `ota_0` | app / ota_0 | 2.94 MiB | 应用镜像 |
| `ota_1` | app / ota_1 | 2.94 MiB | 应用镜像 |
| `assets` | data / spiffs | 2 MiB | 资源数据；源码按标签找到分区并读取/映射资源 |

这张表没有 `recordings`、`userdata` 或其他专用 FAT 用户档案分区。

### 源码路径与存储语义

- `main/settings.cc` 使用 `nvs_open(ns, ...)`、`nvs_get_*`、`nvs_set_*` 和 `nvs_commit` 保存设置。NVS 是键值数据路径，不等于可由文件 UI 导出的一组录音文件；在 meta-pass 当前模型中，默认 `nvs` 也不是每个玩法独占的标签 carve。
- `main/assets.cc` 以 `esp_partition_find_first(..., "assets")` 查找资源分区，读取头部并调用 `esp_partition_read` / `esp_partition_mmap` 访问打包资源。这里不是标准 FAT 录音目录；不能把它当成用户文件系统。
- 语音主要走设备音频采集、网络传输与播放流水线。仅凭“能录入语音”不能推断原始音频会在设备 Flash 中持久化。公开源码没有显示它把每轮对话的麦克风音频作为用户录音文件存进独立 DATA 分区。

### 对 meta-pass 的意义

- 可用于 NVS 持久设置的兼容性观察，但共享 NVS 的命名空间治理与隔离是另一类测试。
- 可用于验证 `assets` 这类标签分区被发现、尺寸与读取边界的分析。
- **不适合作为首选的 DATA 写入/读回/哈希/重启/删除验收固件**：没有找到与 Play 28 等价的独立 FAT 用户数据分区和文件生命周期。

## 2. 电子书：公开开源参考实现

本次检索找到 CHENXiNNNX/Ebook（ESP32-S3 开源阅读器）。这是可读源码的参考项目，**不是已确认的原市场电子书玩法**。

- 源码：https://github.com/CHENXiNNNX/Ebook
- 分区表：https://github.com/CHENXiNNNX/Ebook/blob/main/partitions/partition_16mb.csv
- 用户数据布局说明：https://github.com/CHENXiNNNX/Ebook/blob/main/assets/fat_userdata/README.txt

### 分区与访问路径

| 标签 / 介质 | 格式 / 容量 | 访问方式 | 典型内容 |
|---|---|---|---|
| `userdata` | FAT，6 MiB | `esp_vfs_fat_spiflash_mount_rw_wl("/int", "userdata", ...)` | TXT 书籍、笔记、涂鸦导出等用户内容 |
| `assets` | LittleFS，4 MiB | `esp_vfs_littlefs_register`，只读挂载为 `/assets` | 内置字体、界面与资源 |
| TF 卡 | FAT32，外置 | `esp_vfs_fat_sdmmc_mount`，挂载为 `/sd` | 用户导入书籍及其他文件 |
| `nvs` | NVS，16 KiB | NVS API | Wi-Fi 等设备设置 |

源码中的 `mount_internal_locked()` 按 `userdata` 标签挂载 FAT + wear leveling；`mount_assets_locked()` 按 `assets` 标签挂载只读 LittleFS；阅读器按 `/int/Ebook/txt` 和 `/sd/Ebook/txt` 等固定目录扫描文件。用户文件可以通过 USB MSC、TF 卡或局域网传输。

### 对 meta-pass 的意义

这类实现与 Play 28 在“按 DATA 标签查找文件系统”这一层相似（T1），但生命周期和容量问题更复杂：

- 内部 `userdata` 是 6 MiB，连同约 3 MiB app 不适合直接塞进当前动态池；不能未经容量评估就照搬。
- 阅读内容可能同时位于内部 Flash 和外部 TF 卡；要验证内部 carve 时，必须明确测试文件放在哪个介质。
- 出厂 `userdata` 镜像可以预置书籍，而运行时还会生成笔记/涂鸦/阅读进度缓存；必须分别识别出厂内容和运行时用户数据。
- 不能把这个开源项目的行为直接套到目标市场电子书固件。还需取得市场那款的二进制或对应源码，核对其分区标签和文件路径。

## 3. 三者的测试适配度

| 维度 | 录音笔 Play 28 | AI Passport / DeepSeek 语音 | 开源 Ebook 参考实现 |
|---|---|---|---|
| 独立 DATA 标签分区 | 有：`recordings` FAT | 未发现；主要是共享 NVS + `assets` | 有：`userdata` FAT |
| 用户文件直接可见 | WAV 导出 / 删除 | 未发现本地语音录音文件流程 | 书籍/笔记文件可见 |
| 标签寻址 T1 | 是 | assets 是标签查找，但不是用户 FAT 档案 | 是 |
| 与当前动态池容量匹配 | 缩小到约 4 MiB 后待验证 | app + assets + OTA 方案仍需按实际镜像/分区审查 | 6 MiB userdata + app 不宜直接安装 |
| 适合作为首个真实 DATA E2E | **最适合** | 否（更适合 NVS/assets 专项） | 有价值的源码参考，非目标市场固件 |

## 4. 下载与验证状态

已经定位到公开发布资产及源码，且完成了源码/分区表审查。但当前执行环境无法从 GitHub release 下载二进制 ZIP 到本地（网络 DNS/二进制下载通道不可用），因此**不能声称已完成固件二进制解包、哈希记录或 bin 内字节级行为分析**。后续应在可联网环境下载 `v2.5.0_folotoy-ai-passport.zip`，记录 SHA-256，解包后解析其中的分区表、应用镜像与 assets；然后与对应市场条目固件做 hash/分区/字符串对照。

## 最终决定

继续以 Play 28 作为市场真实 DATA 生命周期验收目标；AI Passport / DeepSeek 语音作为 NVS + assets 的对照案例；开源 Ebook 作为 FAT 用户数据、只读资源和外置 SD 多介质设计的源码参考。电子书市场玩法的具体结论仍待其实际固件证据，不应被开源参考实现替代。
