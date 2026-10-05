<p align="right">
  <a href="dynslot-usb-slot-page-design.md">English</a> · <strong>简体中文</strong>
</p>
# USB 安装页槽位界面：静态槽位 → 动态（dynslot）空间管理

日期：2026-10-05
分支：`main`
父文档：`dynslot-design.md` §4.5（L4 的 USB 侧）、§4.7
相关：`dynslot-usb-installer-design.md`（launcher 升级双模式——另一件事，已设计；
本文覆盖同一页面上的**安装与槽位管理**）

## 1. 问题

USB 串口安装页（`install-slot/install-slot.html`）仍把空间当作三个固定槽位：

1. **第 2 步是写死的三选一**（`Slot 0 @0x180000 / Slot 1 @0x360000 / Slot 2
   @0x560000`）。`applyDiscoveredSlots()` 会按设备分区表（`discoverSlots`）重建
   单选行，所以*几何*是动态的，但页面并不知道哪个槽装了玩法、哪个是空槽、池里
   还剩多少空间，也不知道除分区表声明之外还有多少槽。
2. **不能删槽位。** 设备侧有 `GET /api/install/slots` + `POST
   /api/install/remove`（LAN 通道）；走 USB 时用户完全无法释放槽位，池满了只能
   回到手机端处理。
3. **不会分配空间。** 在*新的*池偏移上写入镜像毫无意义，除非 `store`
   （0x35A000）里的 carve 记录同时增加该槽条目——否则启动器永远看不到这个玩法
   （孤儿字节；下一次安装可能分配到同一偏移把它覆盖）。当前页面只能写到
   表里发现的 `ota_N` 偏移，即从不"创造空间"。
4. **界面没有空间反馈**：没有已用/可用/最大空隙数字，也没有"将在 0x… 新建槽位"
   的预告。

注意：升级 launcher 那一步（第 7 步）已经会探测 dynslot / 固定布局
（`isDynslotLayout`、`comparePartitionTables(.., isDynslot)`），**安装与槽位管理
界面没有跟上**。

## 2. 目标 / 非目标

**目标**

- G1 在 dynslot 设备上，页面从 carve 记录渲染*真实*槽位清单（数量、偏移、尺寸、
  状态、名称、种类）并给出池空间账目。
- G2 用户可以**在页面上删除槽位**：该槽字节失效、槽条目离开记录、0x8000 表被
  重新物化且不再含该槽、行消失，摘要中的可用字节随之增加。
- G3 安装玩法时**动态分配空间**：先复用装得下的空 APP 槽（设备语义
  `meta_carve_find_fit`），否则 first-fit carve 新槽（`meta_carve_place` /
  `dynslot-pool.js:carvePlace`），先写记录、物化表，再流式写镜像——写之前界面
  展示方案（目标行、偏移/尺寸），写之后展示结果。
- G4 用户看到的每个数字、界面提供的每个动作来自**同一决策源**（BUG-20）：由
  记录 + 已载入镜像算出的单一纯视图模型。
- G5 固定槽位 / 原厂设备保持今天的行为（legacy 三选一视图），并有明确的模式
  徽标，使"legacy 兜底"绝不会被误认为"动态探测失败"。
- G6 本地、无硬件验证：mock 设备模式（`?mock=1`）驱动同一份页面代码与同一套
  编解码，跑在内存 flash 上，可在浏览器里、由人先行验证交互，再碰真机。

**非目标**

- N1 不改设备固件。本文全部是页面侧改动；设备契约（记录格式、分配器、hook
  修复）按原样消费。
- N2 不在 USB 侧管理数据 carve（M5）：删除槽位时数据记录原样保留，仍占用自己的
  区间（摘要里单列）。通过 USB 擦除玩法数据分区留作后续。
- N3 不做碎片整理（L1），不改 LAN/手机通道。
- N4 删除时不整区擦字节，只让镜像头失效——理由见 §7.2（与设备端删除一致）。

## 3. 数据源与模式判定

连接成功（`loader` 就绪）后页面读取：

| # | 数据源 | 地址 | 开销 |
|---|---|---|---|
| S1 | 实时分区表 | 0x8000，读 0x1000 | 1 帧 |
| S2 | carve 记录 A/B | 0x35A000 / 0x35B000，各 0x1000 | 2 帧 |

判定顺序（首个命中生效）：

1. **`DYN_SLOT`** — A 或 B 记录可解码（`MPSC` + 版本 + CRC + 结构）。模型 =
   记录；表只用于交叉核对/诊断。
2. **`DYN_FRESH`** — 无有效记录，但 S1 含 `store @0x35A000`
   （`isDynslotLayout`）。模型 = 空 carve（0 槽），每次安装都要 carve。
3. **`LEGACY_FIXED`** — 无记录，S1 含 `ota_0/1/2`。模型 = 表派生槽位（今天的
   `discoverSlots` 行为）；**永不**写记录（该布局下 0x35A000 是未分配空隙，写入
   是被禁止的）。
4. **`LEGACY_FALLBACK`** — 空白/不可解析的表。保持现有 legacy 3 槽兜底并显式
   警告（不变）。

读取失败不致命：页面保留上一次的模式并如实说明（`slot_geo_*` 文案），绝不假装
处于动态模式。

**写入规则**：只有 `DYN_SLOT` / `DYN_FRESH` 才允许写记录。

## 4. JS 侧 carve 记录编解码 —— `install-slot/dynslot-record.js`

新增零依赖 ES 模块（页面与 Node 测试共用），逐字节镜像
`main/meta_carve_store.c` / `main/meta_carve.c`。

记录布局（v2，4KB 扇区；完整字段表见 `meta_carve_store.h`）：

```
[0,16)      "MPSC" | version u16=2 | slot_count u16 | seq u32 | data_count u16 | 0xFFFF
[16,752)    slot[8] × 92B: state u8 | kind u8 | 0xFFFF u16 | offset u32 | size u32 |
            image_len u32 | play_id u32 | sha256[32] | name[40]（0 填充）
[752,3824)  内嵌 carved 分区表（0xC00，自带 MD5 marker）
[3824,4080) data[8] × 32B: play_id u32 | offset u32 | size u32 | state u8 | subtype u8 |
            type u8 | 0xFF | label[16]
[4080,4084) crc32 覆盖 [0,4080)（标准反射 CRC-32，与 zlib 同值）
[4084,4096) 0xFF
```

API：

- `decodeRecord(raw4096)` → `{seq, slots[], data[]}` | `null` —— magic、版本
  （2；v1 布局只读兼容）、count ≤ 8、seq ∉ {0, 0xFFFFFFFF}、CRC、字段范围。
- `pickRecord(rawA, rawB)` → `{rec, fromA, targetSector}` —— seq 新者胜（带符号
  回绕比较，与设备一致）；`targetSector` = 另一个扇区（设备
  `meta_carve_flash_commit` 的轮转），**除非** A 扇区是 `MPCK` Wi-Fi 凭据备份
  （见 §9 E1）→ 改写 B。
- `encodeRecord({seq, slots, data})` → 4096 字节 —— 先 `carveValid()` 校验
  （与 C 同规则：数量/顺序/对齐/粒度/池内/不重叠/kind/state，数据标签 + 保留
  标签 + 可打印 ASCII），物化内嵌表，最后写 CRC。
- `materializeTable(carve)` → 0xC00 —— 固定条目（`nvs/phy_init/factory/cardid/
  store/otadata`，`meta_carve.c:FIXED`）+ 槽位条目（`ota_<i>`，subtype
  `0x10+i`）+ 数据条目，按 offset 升序合并，然后 `0xAA50` 条目 + `0xEBEB`
  marker + 14×0xFF + MD5(entries)。
- `crc32(u8)`、`carveFree`、`carveLargestGap`（几何已存在的部分直接复用
  `dynslot-pool.js`）。

**对拍策略（NO GUESSING 规则）**：JS 编解码只有在与 C / gen_esp32part 输出
逐字节一致的测试（§11 T1–T3）同时成立时才可信。

> 2026-10-05 记：`meta_carve_store.h` 行尾注释写的是 `/* 3776 */` /
> `/* 4032 */`，而宏实际算出 3824 / 4080 —— 本编解码首版照注释写偏移，
> 黄金对拍全红。已随本次改动修正（注释 + `_Static_assert` 钉死）。

## 5. 视图模型 —— 单一决策源（G4）

`buildSlotView({carve, imageLen, mode})` 返回渲染器与动作处理器共用的一切：

```js
{
  mode,                       // DYN_SLOT | DYN_FRESH | LEGACY_*（§3）
  rows: [ { id, slot, offset, size, limit, state, name, kind,
            occupied, targetable, recommended, removeEnabled } ],
  auto:  { enabled, offset, size, limit, reason },      // 新槽提案
  summary: { count, usedSlots, usedData, free, total, largestGap },
}
```

- `targetable`：仅 APP 槽（`kind === app`）且 `imageLen <= limit`
  （`limit = size - 0x1000`，即 `dynslot-pool.js` 的 `appLimit()`）。
- `recommended`:首个**空**且可装行;否则 `auto`(启用时)。与设备
  `suggestedSlot` 同语义 —— 占用槽绝不被推荐(设备 d4209a0 起同样不再建议占用
  槽)。dyn 下两者皆无 → 不默认选中,烧录以 `err_pick_slot_first` 要求显式选择;
  占用槽只有被用户显式选中才会写入,且写入前弹覆盖确认(§6)。删除槽位会让显式
  选择作废(行下标位移),过期下标绝不会指向别的槽。legacy 仍默认首个可装行。
- `auto` 用 `carveNeed(imageLen)` + `carvePlace(occupancy, need)` 在
  `slots ∪ data` 占用域（P1-4）上算；放不下时 `enabled=false`，`reason` 带
  `need`/`largestGap`/`totalFree` 用于错误行（设计 L1：不整理，只解释）。
- legacy 模式下 `rows` 来自 `discoverSlots()`，`auto` 关闭。
- 删除按钮、单选行、摘要读同一份 `rows`。

## 6. 安装流程（G3）

用户点击 Install 时的目标解析：

```
1. model  = 当前视图（记录陈旧则重读）
2. target = 选中行
     auto      → plan = carvePlace(occupancy, carveNeed(imageLen))
                 放不下 → 报错并给 need / largestGap / totalFree
     槽行      → 上限校验（imageLen <= slot.size - 0x1000）
                 已占用(VALID)槽 → confirm()(名称/偏移/大小);
                 取消 → "已取消烧录",零写入
3. 几何步骤（仅当目标是新槽）：
     a. 编码插入新槽（state=EMPTY、seq+1）的记录 → 写目标扇区（4KB 先擦后写）
        → 读回解码 + 结构校验
     b. 把记录内嵌表写到 0x8000（擦 4KB + 写 0xC00）→ 读回逐字节比对
        （记录先行、表随后 —— 与 meta_carve_flash_commit 同序；撕裂写见 §9 E3）
4. 载荷步骤（沿用现有代码路径）：
     wipeSlotResidue(target) → writeFlash(image) → 尾扇区（MSIG/MAEG/MNAM）
5. 元数据步骤：再次提交记录，state=VALID、image_len、sha256、name
   （对应 meta_carve_flash_set_valid）；几何未动 → 不重写表；play_id 原样保留
   （USB 安装的玩法为 0）。
6. 重读记录 → 重建视图 → 日志："Slot N created @0x… · size … · free …"
   或 "Slot N overwritten · free unchanged"。
```

为什么第 3 步先于第 4 步：第 3 步之后断电，留下的是一个**可见的空槽**，用户可以
删掉；第 4 步之后、记录未写则留下没人能看见的孤儿字节（下次分配还可能悄悄覆盖
它）。这与设备自身顺序一致（"新槽在上传前物化"）。

覆盖已有槽时完全跳过第 3 步（几何未动），只做第 5 步的一次元数据提交。覆盖永远是显式行为:占用槽不会被默认选中(§5),被选中后先弹上述确认,再发生任何写入。

## 7. 删除流程（G2）

两步确认（按钮 → `confirm()`，文案含槽位、偏移、尺寸、名称），然后：

```
1. 写 0xFF 擦掉该槽前 4KB（镜像头）
   → 扫描永远不能把它复活成 VALID（设备端防"删除复活"的同一护栏，
     meta_store_install.c h_install_remove）
2. 编码"不含该槽"的记录，seq+1 → 写目标扇区 → 读回
3. 把内嵌表写到 0x8000 → 读回比对
4. 重读记录 → 重建视图（行消失、可用字节变多）
```

头部以外的字节刻意保留：`dynslot-design.md` §4.5 明确"删除时不擦字节，擦除发生在
下次写入"（重装路径本来就在写入前清残留），而且这块空间只通过记录*分配*，记录
已不再引用它。`eraseData` 式的数据分区擦除不在范围（N2）。

顺序理由：先擦后提交与设备一致（擦镜像头 → `meta_carve_flash_remove` → 回 200）。
1 与 2 之间崩溃 → 留下空槽（无害、可删）；2 与 3 之间崩溃 → 记录领先于表，下次
开机由 bootloader hook 修复（§9 E3）。

## 8. 界面改动（`install-slot/install-slot.html`）

- **第 2 步改为槽位表**（仍用 `data-i18n="step_slot"`）：

  ```
  Mode: dynslot (dynamic slots)                       [徽标]
  ┌────┬─────────┬──────────────┬────────┬──────────────┬───────┐
  │ ○  │ Slot 0  │ 0x180000     │ 1.8 MiB│ ● Demo Play  │ Remove│
  │ ○  │ Slot 1  │ 0x360000     │ 2.0 MiB│ (empty)      │ Remove│
  │ ●  │ Auto    │ new @0x560000│ 2.6 MiB│ (new slot)   │  —    │
  └────┴─────────┴──────────────┴────────┴──────────────┴───────┘
  2 slots · used 3.8 MiB (data 64 KiB) · free 2.6 MiB / 6.4 MiB · largest gap 2.6 MiB
  ```

  - 记录里每个槽一行；状态词沿用 `meta_slot_list_word` 的措辞（`(empty)` /
    名称 / `(no firmware)`——不用 "invalid"，见 `meta_slots.h`）。
  - `Auto` 行（仅 dynslot）：载入镜像后显示计划中的新槽 offset/size；放不下时
    禁用并显示 `needs X / largest gap Y`。
  - 每行一个 `Remove` 按钮（仅 dynslot；legacy 模式隐藏）。
  - 摘要行 + 模式徽标：`dynslot (dynamic slots)` / `fixed 3-slot (legacy)` /
    `factory fallback`。
- **第 4 步**按钮不变，状态行额外报出分配结果（新建 vs 复用），让动态决策可见。
- **第 5 步备份勾选**继续由同一模型重建。
- i18n：所有新文案同时进 `I18N.en` 与 `I18N.zh`
  （`tools/install-slot/test-extract.mjs` 强制 key 对齐 + `t()` 覆盖）。
- 任何安装/删除成功后**不重连**即重建视图（从 flash 重读记录）。

## 9. 边界与安全

| # | 场景 | 处理 |
|---|---|---|
| E1 | A 扇区仍是遗留 `MPCK` Wi-Fi 凭据备份 | 绝不覆盖：A 不是有效记录却以 `MPCK` 开头时，`pickRecord` 改定 B 为目标；hook 的 `load_record` 随后会选 B。 |
| E2 | 完全没有记录（`DYN_FRESH`） | 目标扇区 A、seq 从 1 起 —— 与设备 `meta_carve_flash_commit` 在新机上的行为一致。 |
| E3 | 记录撕裂写 / 表撕裂写 | CRC 拒掉撕裂扇区，A/B 中旧者胜；表修复是 bootloader hook 的职责（B5）——页面的顺序（记录 → 表）保证 hook 前提成立。 |
| E4 | 表读回不一致 | 以分层错误中止（`table: read-back mismatch`）；记录已经落盘、hook 下次开机会物化它，因此没有歧义状态。 |
| E5 | 固定/原厂设备 | 完全不读不写记录；legacy 界面（G5）。 |
| E6 | 记录有槽、表不一致（中间态） | 模型以记录为准；下一次写入时第 3/7 步重物化表。 |
| E7 | storage 种类槽（`kind=storage`） | 显示但 `targetable=false`，可删除（释放预留正是 L2 的意义）。 |
| E8 | 池满 / 碎片 | `auto.enabled=false` + `needs/largestGap/totalFree`（L1：解释，绝不偷偷整理）。 |
| E9 | 操作中断线 | 现有 `markDisconnected` 路径；每步都可从"重读记录"重新推导，重试安全。 |

## 10. mock 设备模式（G6）

`?mock=1`（静态托管可用 `#mock`）把页面切到模拟设备：

- 新模块 `install-slot/mock-device.js`：8MB 内存 flash，用
  `dynslot-record.js` 生成的*carved*表 + 有效记录做种子（2 个槽：一个 VALID 带名
  玩法、一个 EMPTY，外加一条数据记录），并实现与 esptool 一致的
  `readFlash(offset, size, progress)` / `writeFlash({fileArray, reportProgress})`
  语义（按扇区先擦后写，与 ROM/stub 行为相同）。
- `sync/connect/runStub/eraseFlash` 为空实现，使所有现存代码路径（含读恢复）原样
  运行。
- Connect 完全跳过 `navigator.serial`，芯片报 `MOCK ESP32-C3`；页面顶部常驻横幅
  标明"模拟"，模拟会话绝不会被误当成真机会话。
- mock 用与生产流程*同一套*编解码存字节，因此浏览器走查（连接 → 列表 → 删除 →
  安装 → 列表）是真正的字节级往返，而不是 UI 桩。
- Node 可以 import 同一模块，因此交互序列也由 host 测试覆盖（§11 T6）。

## 11. 测试计划

| # | 测试 | 工具 |
|---|---|---|
| T1 | `materializeTable` == `carve_migration_table.bin` / `carve_shrunk_table.bin` / `safe_table.bin`（gen_esp32part 字节权威；与 C 测试 `tests/test_meta_carve.c` 同一组黄金产物） | `tools/install-slot/test-dynslot-record.mjs` |
| T2 | decode(C 生成的黄金记录) → 再 encode → **逐字节相同**；几何/名称/sha/数据断言钉死 | 同上 |
| T3 | CRC 黄金（`"123456789"` → `0xCBF43926`）；拒绝：坏 magic / 坏版本 / CRC 篡改 / count>8 / 非法 state | 同上 |
| T4 | `pickRecord`：seq 新者胜、回绕、A/B 轮转、`MPCK` 护栏 | 同上 |
| T5 | `buildSlotView`：复用优先推荐、auto 提案、禁用 auto 的数字、storage 行、legacy 行、摘要计算 | 同上 |
| T6 | mock 设备交互：种子 → 安装（新槽）→ 记录/表/字节一致 → 删除 → 行与空间恢复 | 同上 |
| T7 | 新增文案的 i18n key 对齐 / `t()` 覆盖 | 既有 `tools/install-slot/test-extract.mjs` |
| T8 | 浏览器 mock 模式走查（列表、删除、安装、摘要、横幅） | 本地服务器 + 浏览器，截图 |
| T9 | 真机：dynslot 安装 → 启动器列出玩法；删除 → 玩法消失 + 可用空间增加；固定槽位设备行为不变 | 人工清单（§12） |

门禁：`./tools/validate.sh --static` 必须保持绿色（host 测试 + 仓库检查）。

## 12. 真机验证清单

1. **dynslot 设备 · 动态列表**：按住 UP 开机，Connect → 第 2 步出现 `dynslot`
   徽标，各行与设备屏幕上看到的玩法一致（每台设备偏移/尺寸不同），摘要可用字节
   与启动器"剩余空间"行一致。
2. **动态分配**：选一个比所有空槽上限都大的本地 `.bin`（或先把池装满）→ `Auto`
   行显示计划偏移；Install → 日志出现 `record: seq N→N+1`、`table: materialized`、
   镜像写进度；重启 → 启动器在该偏移列出新玩法。
3. **删除**：在某个玩法行点 Remove → 确认 → 行消失、可用空间增加该槽尺寸；重启
   → 启动器不再列出它；再装另一个玩法时落在刚释放的空隙（用日志偏移核对）。
4. **legacy 固定槽位设备**：页面显示 `fixed 3-slot (legacy)` 徽标、三个单选、无
   Remove 按钮；安装行为与之前完全一致。
5. **断电演练（可选）**：记录写入时拔电 → 页面报写失败；重新上电 → 设备启动
   （hook 修复），槽位清单等于 A/B 两个状态之一。

交付时必须报告的字段（按 AGENTS.md）：

```
Build:        PASS / FAIL / NOT RUN
Host tests:   PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified:   <尚未验证项>
```

## 13. 实施顺序

1. `install-slot/dynslot-record.js`（编解码 + 规划 + 视图模型）——纯逻辑。
2. 生成 C 侧黄金夹具 + `tools/install-slot/test-dynslot-record.mjs`，登记进
   `tools/validate.sh`。
3. 页面接线：连接时模式判定、槽位表 UI、删除动作、安装目标解析、i18n。
4. `install-slot/mock-device.js` + `?mock=1` 接线。
5. `./tools/validate.sh --static`，然后浏览器走查（T8）与真机清单（T9）。
6. 依据人工反馈迭代；用户可见改动记入 `docs/CHANGELOG.md`。

## 14. 待决问题

- **Q1** 深度清理选项（"连字节一起擦"）——暂缓；当前行为与设备删除路径和回收
  阶梯一致（§7.2）。若现场反馈显示残留字节造成困惑，再加勾选项。
- **Q2** 从 USB 页删除玩法的*数据*分区（N2）——需要给 USB 安装的玩法一个
  `play_id` 故事（当前记为 0）。
- **Q3** 池碎片化时的「自动分配」诉求 —— 总可用够但最大空洞不足时,当前按
  L1 只解释不整理(禁用 Auto 并给出 need/largestGap/totalFree)。若现场确有需求,
  后续做**显式**搬槽/整理(读 → 搬 → 擦 → 提交记录,绝不静默执行),或引导用户
  先删除占用槽腾出连续空间。
