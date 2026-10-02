<p align="right">
  <a href="dynslot-design.md">English</a> · <strong>简体中文</strong>
</p>

# 动态分区（dynslot）设计

日期：2026-10-02
分支：`feat/dynslot`（设计提案 —— 尚无实现）
证据基础：`docs/assets/play563-appstore-download-reverse.md`、对线上 play 563
二进制的字节级验证、ESP-IDF 5.5.3 源码、当前 meta-pass 代码树
（`main` @ `9e591a2`）。

## 1. 问题

槽位几何被冻结为三个固定大小的分区：

| 槽位 | 分区 | 原始大小 | 可用大小（`size − 4KB` 尾扇区） |
|---|---|---:|---:|
| 0 | ota_0 @ 0x180000 | 0x1D6000（1,896,448 B） | 1,892,352 |
| 1 | ota_1 @ 0x360000 | 0x200000（2,097,152 B） | 2,093,056 |
| 2 | ota_2 @ 0x560000 | 0x29E000（2,720,512 B） | 2,716,416 |

适配是逐槽位的悬崖：镜像必须装进*某一个*分区，即使各槽空闲空间之和足
够。以下案例今天会失败、在池化布局下可以装下（数字均已实测，见 §2）：

- 两个各约 3.2 MB 的应用（play 563 自身 factory 在其内嵌表中就是 3M 分
  区）：需要 6,447,104 B——今天不可能（单槽最大可用 2,716,416 B），在
  6.45 MiB 的池中轻而易举。
- 2,740,224 + 1,844,496 + 1,200,000 B 三者并存：今天失败
  （2,740,224 只能进槽 2，之后 1,844,496 只能进槽 1），池化下可以紧凑
  排布。

固定表的其他代价：几何信息被复制在三处且必须保持同步
（`partitions.csv`、`install-slot/store-analyze.js` 的 `SLOT_GEOMETRY`、
`install-slot/install-slot.html` 的 `SLOTS`），发布门还通过
`UPGRADE_PRESERVED_REGIONS` 全 0xFF 检查把它冻结
（`tools/verify_firmware.py:34-40`）。

目标：用**由启动器管理、由 bootloader hook 强制执行的 flash 池 + 可变大
小槽位**取代固定槽位。

## 2. play 563 的做法（已验证，以及我们借鉴什么）

对线上二进制（`/api/download/community/ai-passport-9`，SHA-256
`2956f77b…3fbdf01`，与目录元数据一致）和 ESP-IDF 5.5.3 源码的字节级验
证：

1. **被下载的玩法自带分区表。** 0x8000 处的合并镜像解码为：nvs 24K、
   phy_init 4K、factory 3M、otadata 8K、cardid 16K、ota_0 3M、store 16K
   （NVS）、easter 388K、recovery 1M。
2. **分区表 MD5 格式**（对照二进制与 `gen_esp32part.py:411` 验证）：
   `md5(各条目)` 紧跟在 `0xEBEB + 0xFF×14` 终止符之后——*不是*整个扇
   区。因此运行时重写分区表是一个完全可复现的、有规范的字节格式。
3. **安装器从内嵌表生成写入区间计划并直接写 flash**（据此前的反汇编：
   `store_ota`、2,048 字节分块、流式 SHA-256、分区表写入重试 3 次）。
4. **集中的元数据分区（`store`，16K NVS）记录已安装内容**——槽位状态
   存放在应用镜像之外。
5. **对先前逆向文档的更正：** recovery 位于子类型 0x20，bootloader 将其
   归类为 "Unknown app"，*不是* OTA 槽位（`bootloader_utility.c` 判定条
   件为 `(subtype & ~0x0F) == 0x10`；掩码为 0x0F，
   `esp_flash_partitions.h:21-22`）。只有子类型 0x10–0x1F——**16 个 OTA 槽
   位**——可被 otadata 选中。
6. 无 Range/断点续传，单次直连源站下载——只有快路径。meta-pass 保留自己
   的 Range/续传机制，不借鉴。

借鉴的机制：**按已验证 MD5 格式进行运行时分区表物化**、**集中于镜像外
的槽位元数据**、**由管理者（启动器）而非子固件拥有布局决策权**。

不借鉴：play 563 用子固件自己的表*整体替换*分区表，这会驱逐启动器。
meta-pass 的启动器必须在每次安装后存活，因此 carve 表由启动器计算、且
被约束在池内。

## 3. 硬性约束（均已验证）

- IDF 二级 bootloader 只启动 0x8000 表中声明的分区，且每次启动都校验
  MD5（`bootloader_utility.c:158` → `esp_partition_table_verify`）。
- otadata 槽位选择不依赖固定数量：`slot = (ota_seq−1) % ota_count`
  （`bootloader_utility.c:412-415`），现有 hook 已在运行时扫描表获取
  otadata 地址与槽位数量（`bootloader_components/meta_boot_hooks/hooks.c:69-72`）。
- 受保护、不可变：factory @ 0x10000 ≤ 0x170000、cardid @ 0x356000/0x4000
  （`tools/verify_firmware.py:22-24`）。
- 无 secure boot、无 flash 加密（`sdkconfig.defaults`）→ 允许运行时写分
  区表。
- 系统级策略必须在不可绕过的层强制执行（AGENTS.md:20）→ 执行点是现有
  的 bootloader hook，而非子固件配合。
- app 分区偏移必须 64 KB 对齐；大小为 4 KB 的倍数。

## 4. 设计

### 4.1 物理模型：池 + 划分（carve）

cardid 之后的应用空间成为逻辑上的一体池、物理上的两段（cardid 无法被
分区跨越）：

- pool_0：[0x180000, 0x356000) —— 0x1D6000（1,896,448 B）
- pool_1：[0x360000, 0x7FE000) —— 0x49E000（4,841,472 B）
- **池总计：6,766,592 B（6.45 MiB）**——对比今天可用 6,754,304 B；收益
  是分配灵活性而非容量（多花的 ≈45 KB 是 ≤8 个尾扇区 + 24 KB store）。

无主的 24 KB 对齐缝隙 0x35A000–0x360000（今天是未经分区的 Wi-Fi 凭据
原始备份区，`main/meta_store_net.c:119-120`）变为 **store 分区**
（`store`，data/NVS，0x35A000/0x6000）。凭据备份迁入 store 区内部
（§6，L6）。

分配器规则：

- 槽位偏移 64 KB 对齐；大小按 4 KB 取整；**最小槽位 128 KB**。
- **最多 8 个槽位**（IDF 硬上限是 16 个 OTA 子类型；8 是设计上限）。
- 先 pool_0 后 pool_1，first-fit。小段优先是刻意策略：pool_0（约
  1.8 MB，被 cardid 切断）先吸收小槽位，让 pool_1 保留大连续跨度给大应
  用/大档案。v1 不做压缩整理（§6，L1）。

### 4.2 三种表状态

1. **安全表（safe table）**——编译进 factory 镜像、嵌入 store、并随发布
   产物置于 0x8000：nvs、phy_init、factory、cardid、store、otadata，再
   加上两段池区域声明为 *data* 分区（不可启动的占位符）。保证最糟情况
   下设备仍可启动；损坏的 carve 永远不会让垃圾镜像变得可启动。
2. **carve 表**——安全表条目加上每个存活槽位一个 app 条目
   （ota_0..ota_n），偏移取 carve 值，按已验证的 MD5 格式逐字节生成
   （`md5(条目)` + `0xEBEB 0xFF×14` + 摘要）。
3. **启动时不存在第三种状态**——bootloader hook（§4.4）在 otadata 被采
   信之前修复任何偏离。

### 4.3 carve 元数据（`store` 分区）

A/B 双记录（seq + CRC32，先擦后写，即 `meta_boot_policy.h` 已有的模
式）：

```
magic, format_version, seq, slot_count,
slot[i] = { state, offset, size, image_len, image_sha256, name[40] }
crc32
```

外加一份安全表副本和一个草稿扇区：24 KB 足够。槽位状态与扫描状态一致
（EMPTY/VALID/INVALID，`main/meta_slots.h:14-18`）。启动器每次启动都对
每条记录对照 flash 重新校验（SILENT 镜像校验 + 尾扇区，即现有
`meta_store_scan` 逻辑）并回收死记录——派生状态保持派生；store 只是意
图的缓存，绝不盲信。

### 4.4 启动与校验流程（执行层）

```text
ROM → 二级 bootloader
  │  IDF 校验 0x8000 表 MD5
  ▼
meta_boot_hooks（扩展）
  │  读 store A/B → 最新有效 seq（CRC 正确）
  │  比对现表条目与已提交 carve：
  │    一致            → 继续
  │    不一致/损坏     → 用 store 中的副本重写 0x8000
  │                      （store 失效 → 写编译进的安全表），
  │                      擦除 otadata，启动 factory
  ▼
otadata 选择（IDF 语义不变）
  冷启动 ⇒ otadata 为空（单会话策略，hooks.c:106-128）
  ⇒ factory / 启动器
  ▼
启动器 app_main
  表 ≠ 已提交 carve ? 物化 carve（未变化时为空操作） : —
  从 carve 表扫描槽位（esp_partition_find_first，不变）
  用户选择玩法：
    carve 未变 → esp_ota_set_boot_partition(ota_i) + restart
    carve 改变 → 提交 store A/B → 物化表 → 设置 otadata
                 → restart；hook 在 otadata 被采信前校验 carve
```

每次影响 carve 的安装/移除：多一次重启。在现有 carve 下启动玩法：零额
外重启（与今天相同）。

### 4.5 安装/移除路径的改动

- **analyze**：`SLOT_GEOMETRY` 被唯一的池描述符取代
  （`ranges, min_granule, min_slot, max_slots, tail_sector`），放在共享
  JS 模块中供 `store-analyze.js`、`phone-install.js`、`install-slot.html`
  消费——几何副本从三处减到一处，并保留与今天相同的设备权威互锁
  （`meta_install_model_offer_ok` 拒绝设备无法在本地复现的手机侧提案，
  `main/meta_install_model.h:83-84`）。
- **提案**：手机端计算建议 carve（first-fit，或复用空槽）；设备端重跑分配
  器，不一致即拒绝。
- **写入路径不变**：提取后的 app 镜像、`esp_ota_begin/write/end`、流式
  SHA-256、带 MSIG/MNAM/MAEG 的尾扇区
  （`main/meta_store_install.c:466-621`）。唯一区别是分区来自 carve 而非
  固定子类型。
- **移除**：在 store 中标记空闲；重新物化表时不含该条目。数据不移动
  （v1）。
- **空闲空间回收阶梯**（删除固件留下的空洞如何处理）：
  1. **可切分的空闲区间。** 删除玩法即释放其应用槽与数据 carve 记
     录；区间回到 store 元数据的空闲池。由于 carve 边界按分配重算（不
     是固定槽位），释放出的 2.6 MB 空洞可以装下一个 1.5 MB 应用 + 一
     块 1 MB 数据 carve——这是固定槽架构做不到的复用。删除时不擦
     除；擦除发生在下次写入时，与今天一致。
  2. **first-fit**，先 pool_0 后 pool_1，剩余 ≥ 128 KB 保留为空闲区
     间。
  3. **分配失败时，回收 ARCHIVED 数据记录**（M5：卸载时保留的档
     案，最旧先回收），擦除后重试适配。
  4. **最后手段，PRISTINE 数据记录**：内容可从玩法的安装镜像复现，
     因此可以丢弃记录、日后重新恢复（需手机重发镜像）；以此方式丢
     弃在用玩法数据前需经用户确认。
  5. **拒绝**，原因码 `no-fit` 并给出数字（需求 vs. 最大空闲区
     间）——UI 可提示"请先移除/归档 X"。
  flash 内拷贝压缩（搬动在用槽位以合并非相邻空洞）与原位扩容**刻意放
  在 v2**：它们需要自己的 copy-then-commit 崩溃恢复故事，而 v1 的可切
  分区间 + 归档回收阶梯已覆盖真实目录场景。

### 4.6 迁移

- 带旧版 v1.x 表的设备：启动器检测到固定三槽表，在正常扫描后*以相同*
  偏移/大小播种 carve，提交 store，物化（逐字节看是空操作），继续运
  行。已安装玩法不受影响。
- 发布门：`verify_firmware.py` 的保留区域从 nvs/ota_0/ota_1/ota_2/otadata
  改为 nvs/store/otadata + 两段池（产物中均为全 0xFF，使用户数据在启动
  器升级后存活）。

### 4.7 故障模式矩阵（断电）

| 中断点 | 结果 |
|---|---|
| 镜像写入中途 | 槽位 INVALID——与今天语义相同，不变 |
| store 提交中途 | A/B seq → 较旧的有效记录获胜 |
| 表写入中途（单个 4 KB 扇区） | 下次启动：hook 用 store 副本恢复表（或安全表）；擦除 otadata → factory |
| store 分区损坏 | hook 写编译进的安全表 → factory；池内容完好但无法寻址 → 玩法需重装（已接受，L7） |
| 子固件涂写池区域 | 损害被限制在池内；hook 恢复任何触碰 cardid/factory/otadata 的表；子固件无法自行启动（冷启动擦 otadata） |

## 5. 设计边界（硬性）

- B1：`cardid` @ 0x356000、`factory` @ 0x10000/0x170000、`otadata` @
  0x7FE000 不可变。dynslot 只重新划分池。
- B2：≤8 个槽位、64 KB 偏移对齐、4 KB 大小粒度、128 KB 最小槽位。IDF
  上限是 16 个 OTA 子类型；我们不逼近它。
- B3：总安装容量 ≈ 6.45 MiB 减去各槽尾扇区——容量没有奇迹；赢的是分配
  灵活性。
- B4：每次影响 carve 的安装/移除一次重启；carve 未变时启动玩法零额外
  重启。
- B5：bootloader hook 是权威；与已提交 store 元数据不一致的 carve 表会
  在 otadata 被读取前被修复。
- B6：子固件契约不变——玩法看到的仍是普通 OTA 分区
  （`esp_ota_get_boot_partition`），仍通过 OK 长按返回
  （`metapass_return_to_launcher`）。

## 6. 功能限制

- L1：**v1 不做压缩整理。** first-fit 仍可能拒绝一个整理碎片后装得下
  的镜像；用户需移除其他玩法。压缩（flash 内拷贝 + 重新 carve）是后续
  阶段。
- L2：**ota_2 的 littlefs 双用作为固定属性消失。** 今天"录音固件在无合
  法镜像时把 ota_2 挂载为 littlefs"（meta-pass-design.md §6.3）变为
  carve 中一条可选的 `storage` 类型记录；需要录音的设备在 carve 时预留
  一块。默认 carve：全为应用。
- L3：硬编码槽位偏移的玩法（违反契约）在 dynslot 下会损坏。
- L4：手机端与 USB 端安装器必须先采用共享分配器，固件才启用 dynslot；
  manifest 版本会拦截旧 analyze/新固件的混合组合（设备拒绝过时的几何提
  案）。
- L5：每个槽位仍消耗 4 KB 尾扇区（MSIG/MNAM/MAEG 与签名机制不变）。
- L6：Wi-Fi 凭据原始备份从 0x35A000–0x360000 迁入 `store` 内的专用区
  域；迁移时旧备份丢失（需重新输入一次 Wi-Fi 凭据）。
- L7：store 分区损坏会丢失槽位表（玩法仍在 flash 中但不可达）；恢复方
  式是重装，不是修复。
- L8：从 dynslot 降级回 v1.x 启动器，仅在 carve 仍等于旧三槽偏移时安
  全；任何调整大小之后降级需要重刷。

## 7. 范围之外（后续工作，不属于本设计）

- 压缩/垃圾回收与原位槽位调整大小。
- 超过 8 个槽位；移动 otadata；跨越 cardid。
- 池加密；secure boot 交互（两者都关闭时无关紧要）。

## 8. 验证计划

- 主机纯逻辑测试：分配器（first-fit、对齐、最小/最大槽位）、carve↔表
  物化字节对比 `gen_esp32part.py` 输出、MD5 黄金向量（含 play 563 表字
  节）、从旧表固件播种迁移、故障矩阵状态迁移。
- bootloader hook 测试（QEMU）：表扇区损坏、store A/B 损坏、carve/表不
  一致、子固件涂改的表——每种情况都必须落到 factory。
- 设备端 E2E：安装 §1 中的激励案例；在矩阵每个阶段注入断电。
- 门禁：`tools/validate.sh --static`；更新 `tests/test_verify_firmware.py`；
  §4.6 的产物契约。

## 9. 待决项（实现前需确认）

1. 最大槽位 = 8、最小槽位 = 128 KB——确认或调整。
2. `storage` carve 类型：保留（L2）还是彻底放弃录音双用？
3. Wi-Fi 凭据备份迁入 `store`（L6）——是否接受？
4. otadata 保持在 0x7FE000（建议；移动它没有收益）。
5. 接受"每次影响 carve 的安装/移除多一次重启"（B4）？
