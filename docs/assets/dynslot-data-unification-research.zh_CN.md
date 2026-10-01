<p align="right">
  <a href="dynslot-data-unification-research.md">English</a> · <strong>简体中文</strong>
</p>

# 子固件数据存储：接管/映射可行性调研

日期：2026-10-02
分支：`feat/dynslot`
配套文档：`dynslot-design.md`（池 + carve）。本文回答一个问题：
**meta-pass 能否接管/统一子固件（玩法）的数据存放位置，以便集中管
理存储空间与数据共享？**

## 1. 结论（先说答案）

**部分可行——三个层级，不做运行时拦截。**

| 层级 | 访问模式 | 可行？ | 机制 |
|---|---|---|---|
| T1 | 按**分区标签**寻址的数据（NVS 分区标签、SPIFFS/littlefs 按标签挂载、`esp_partition_find_first(label)`） | **可行，子固件零改动** | carve 时标签别名：carve 表以相同标签、池内偏移声明子固件自己声明的数据分区（§4，M1） |
| T2 | 默认共享 `nvs` 分区（按命名空间存键） | **可管理，不可重定向** | 命名空间治理：约定 + `nvs_entry_find(part, NULL, …)` 枚举/清除（§4，M2） |
| T3 | **固定偏移**裸 `spi_flash_*` / `esp_flash_*` 访问 | **不可行** | 运行中的子固件与 flash 之间没有任何环节。只能靠契约层面缓解（§4，M4 已否决） |

对运行中子固件的存储调用做运行时拦截（VFS 挂钩、`spi_flash` 驱动打
补丁、cache-MMU 重映射）**否决**：子固件链接自己的 ESP-IDF 副本、注册
自己的 VFS、拥有自己的 MMU 映射。没有受支持的拦截点；而 dynslot 已
经把唯一真正掌管布局的层——分区表——交给了 meta-pass。

## 2. 玩法的实际做法（实测）

### 2.1 生态抽样：六个玩法二进制字节级解码

| 玩法 | factory 大小 | 声明的数据分区 | 今天可否作为子固件安装？ |
|---|---|---|---|
| 赛事看板 (829) | **8,128K** | 无 | 否——超过任何槽位 |
| 钓鱼图鉴 (733) | **8,128K** | 无 | 否 |
| 屁屁侦探 (826) | **8,128K** | 无 | 否 |
| RELAY 通话器 (842) | **8,128K** | 无（nvs 缩为 16K） | 否 |
| 像素工牌 (523) | 7,936K | `content` data 192K @ 0x7D0000 | 否 |
| AppStore (563) | 3,072K | `store` NVS 16K + `easter` 388K | 是 |

由此确立的事实：

1. **大多数玩法是整盘独立镜像**（factory ≈ 8 MB，无 OTA 槽位，无
   cardid）。它们从 0x0 整体烧录、独占设备；不重建就不是 meta-pass
   意义上的子固件。只有按槽位模板构建的玩法（play 563 是参考）才是
   可安装的子固件。
2. **可安装风格的玩法中，声明数据分区很罕见**——抽样的只有 play 563
   的 `store`/`easter`；像素工牌带 192K `content` 数据分区。数据管理
   是真实但面向未来的问题。
3. **数据分区的*内容*当前被 meta-pass 安装流程静默丢弃**：
   `install-slot/extract-app-image.js` 只提取 factory 应用镜像；子固件
   的数据分区从不解析、从不安装。任何数据统一方案都顺带修复这个有损
   缺口。
4. 参考玩法 Voice-Keychain 把 SPIFFS `voicefs` 挂载到 `/voices`，通过
   `esp_vfs_spiffs_register` 按分区标签挂载
   （`official-main/docs/reference/shinku-chen/voice-keychain/voice-guide.md:38-42`）
   ——这是典型的按标签寻址模式（T1）。

### 2.2 上游模板事实

- 模板分区表（`official-main/partitions.csv:1-5`）：nvs 24K、
  phy_init、factory 3M、cardid。无 storage/data 分区。
- **上游代码中零 NVS 应用使用**（official-main 全库无 `nvs_open`）。
  没有命名空间约定；每个玩法各自发明。
- 模板录音只驻留 RAM（`official-main/main/demo_audio.c:60-89`）；
  flash 持久化录音只是文档里的说法，不是代码
  （`official-main/docs/README.md:44-45`）。

### 2.3 meta-pass 自身的存储（启动器也是自己分区表的"子固件"）

- 全部走分区 API：`esp_partition_find_first` 按子类型查找
  （`main/meta_store.c:19-25`）——槽位镜像、尾扇区（MSIG/MAEG/MNAM，
  镜像内相对偏移）、otadata。在 carve 下全部天然可重定向。
- **唯一的固定偏移违规者**：Wi-Fi 凭据备份，硬编码 `0x35A000`（MPCK
  magic + crc32，`main/meta_store_net.c:119-178`）——属 T3，dynslot 下
  迁入 `store` 区（dynslot-design.md 的 L6）。
- NVS `metapass` 命名空间存 Wi-Fi 凭据；已记录的脆弱点：子固件启动可
  以格式化共享 nvs 分区（`main/meta_store_net.c:119-124`）。
- 子固件契约（`main/metapass_hook.h`）**不提供任何存储 API**——只有
  签名校验自诊断和返回启动器。

## 3. 为什么运行时拦截不可行

| 候选方案 | 失败原因 |
|---|---|
| VFS 挂钩 | VFS 是按注册项组织的驱动表；子固件用自己的 ID 注册自己的挂载点。没有全局抢占点。 |
| `spi_flash`/`esp_flash` 打补丁 | 子固件链接自己的 IDF；符号在子固件内部解析。无受支持的覆盖手段。 |
| Cache-MMU 重映射 | 映射由 IDF 启动流程为应用自身段建立；按子固件重映射 flash 区域不是受支持的配置，且与 bootloader 自身的映射冲突。 |
| 用 flash 加密当闸门 | 会整体改变信任模型；secure boot 与 flash 加密都是关闭状态，本身就是独立项目。 |

且按 AGENTS.md:20，任何*强制执行*必须位于 bootloader/校验器层——在
dynslot 下那正是 carve 表 + `meta_boot_hooks` 校验。那一层掌管的是
**布局**，不是运行时调用；下面的设计就停留在那一层。

## 4. 可行机制

### M1 — carve 时标签别名（T1，主机制）

子固件的合并镜像已经声明了完整分区表；手机端 analyze
（`extract-app-image.js`）已经解析它。扩展 analyze，输出子固件的**数
据分区需求** `{label, type, subtype, size}`。carve 随后：

1. 为每个声明的数据分区预留池空间；
2. 以**相同标签/类型**在池内偏移物化表条目（偏移按 carve 而定，跨槽
   冲突不可能发生）；
3. 随应用镜像一起安装数据分区的*内容*（修复 §2.1-3 的有损缺口）；
4. 全部记入 store 元数据，成为每槽**数据 carve 记录**
   `{label, type, offset, size, sha256}`。

子固件**无需任何改动**：它用 `esp_partition_find_first(DATA, x,
"store")` / `esp_vfs_littlefs_register` 和自己标签交互，拿到的就是真
实分区。已验证的 IDF 行为保证这一点的可靠性：分区查找按加载顺序遍
历表，`esp_partition_find_first` 返回首个匹配
（`esp-idf-v5.5.3/components/esp_partition/partition.c:359-381`）；
carve 条目使用各子固件自己的标签，因此不存在别名歧义——唯一不能按槽
复制的标签是默认 `nvs`（T2）。

数据一旦纳入 carve，管理收益随之而来：卸载时按玩法擦除数据（今天：
数据成孤儿）、store 元数据中的每玩法配额、数据随镜像备份/恢复、
carve 压缩整理时随槽位迁移。

### M2 — 共享 `nvs` 治理（T2）

默认 24K `nvs` 分区无法按槽重定向（单一标签，首个匹配生效）。改为
管理：

- **约定**：子固件使用命名空间 `play:<slug>`；meta-pass 保留
  `metapass`（系统）与 `play:*` 前缀。
- **枚举强制执行**：`nvs_entry_find(part_name, NULL, NVS_TYPE_ANY,
  &it)` 可遍历所有命名空间的所有条目（IDF 5.5.3
  `nvs_flash/include/nvs.h:716`）；启动器可在启动时和卸载时列出并删
  除非白名单命名空间（`nvs_erase_key`/命名空间清理）。不守规矩的子固
  件爆炸半径被限制在 24K 内。
- **已知风险仍部分开放**：子固件在 `nvs_flash_init()` 出错时*格式化*
  nvs，会连带抹掉系统凭据（`meta_store_net.c:119-124`）。MPCK 裸备份
  已覆盖此场景；dynslot 把该备份迁入 `store` 后安全网保留。

### M3 — 可选的子固件 SDK 数据 API（T2/T1 便利层，基于配合）

扩展 `metapass_hook.h`，增加可选数据层（按标签约定返回本玩法 carve
的挂载助手、以本玩法 NVS carve 为后端的 KV API）。人机工程更好，也
是挂配额/共享逻辑的挂载点，但纯自愿——仅适配子固件可用。M1/M2 不依
赖它。

### M4 — 跨玩法数据的共享区

一个标签固定的 `share` 数据分区（如 256K–1M，carve 时固定一块，不属
于任何槽位），存放真正跨玩法的数据（录音、公共素材）。靠约定与文档；
子固件按固定标签寻址。其尺寸是 carve 时决策，会占用池空间——默认做
小并可选。

## 5. 统一模型（dynslot store 元数据的 schema 增量）

```
slot[i] += data_count, data[j] = { label, type, subtype, offset, size, sha256 }
carve  += 可选固定记录: { label:"share", ... }
```

启动器生命周期钩子：安装（M1 别名 + 内容恢复）、卸载（擦除该槽的数
据 carve 记录；可选归档）、启动（M2 下做 NVS 白名单清扫）、迁移
（dynslot 压缩整理时数据记录随槽位移动）。

## 6. 边界与限制

- D1：T3（固定偏移子固件）无法映射、无法可靠检测、无法在池之外加以
  限制。生态契约必须继续劝阻裸偏移；meta-pass 自己迁走唯一的违规者
  （MPCK）。
- D2：整盘独立玩法（factory 约 8 MB，目录中的大多数）完全在子固件数
  据管理范围之外——它们不是子固件。
- D3：无命名空间纪律的默认 `nvs` 子固件与系统凭据共享 24K；M2 限制
  但不能根除风险。根除需要子固件采用 M1 式自有分区或 M3。
- D4：运行中的子固件拥有完整 flash 访问权；carve 治理是布局权威，不
  是沙箱。信任级别与今天相同。
- D5：数据 carve 消耗池空间；声明大数据分区的子固件会压缩应用可用来
  余量。配额与 analyze 时拒绝机制适用。
- D6：统一模型的强度取决于 analyze 对子固件内嵌表的解析——畸形的子
  固件表在安装时被拒绝（现有 format 门），不做重新解释。

## 7. 建议

1. **数据 carve 记录**现在就纳入 dynslot store 元数据 schema（预留字
   段，零成本）。
2. 随 dynslot v1 落地 **M1**——就是同一个 carve 机制，只是多几种条目
   类型；顺带修复数据分区被丢弃的缺口。
3. 随 dynslot v1 落地 **M2**（命名空间白名单清扫）——成本低，限制已
   知的 nvs 风险。
4. **M3/M4** 作为后续；**运行时拦截：永不**。
