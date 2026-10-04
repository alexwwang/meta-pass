<p align="right">
  <a href="dynslot-usb-installer-design.md">English</a> · <strong>简体中文</strong>
</p>
# USB 安装器双模式：动态槽位 vs 固定槽位

日期：2026-10-03
分支：`feat/dynslot`
父文档：`dynslot-design.md` §4.5 L4

## 1. 问题

USB 安装器（`launcher-upgrade.js` + `install-slot.html` 第 7 步）
当前假设单一升级路径：写入 bootloader + 分区表 + factory app + otadata
到固定偏移，然后将设备的实时分区表与包内分区表逐字节比较。这适用于
固定槽位设备，但对 dynslot 设备会失败，因为：

1. **分区表结构不同**：固定槽位设备声明 `ota_0/1/2` 为应用分区；
   dynslot 设备声明 `pool_0/1` 为数据占位符，由 launcher 在运行时
   物化 carve 表。逐字节比较必然失败。
2. **升级范围不同**：dynslot 设备必须保留整个池（包括 store 中的
   carve 表快照），而固定槽位设备只保留三个 ota_* 区域。
3. **擦除计划不同**：固定槽位迁移擦除 `0x180000→0x7FE000`；
   dynslot 迁移只擦除遗留残留（如果有），同时保留池内容。
4. **无版本信号**：手机无法在提交升级路径前知道连接的设备运行的是
   固定槽位还是 dynslot 固件。

目标：使 USB 安装器支持两种模式，由设备固件版本（或等效的能力探测）
选择，让用户无需切换工具即可从任何版本升级。

## 2. 设计决策（已决议）

| # | 决策 | 理由 |
|---|------|------|
| 1 | **通过 `/api/install/slots` 探测**（非 HTTP 头部 / 新端点） | 现有调用；JSON 响应添加一个 version 字段；向后兼容（parseSlots 忽略未知字段）。 |
| 2 | **以 `protocol_version` 整数门控**（非 semver 字符串） | 整数无歧义、易解析、无日期/版本号格式边界情况。固定槽位 = `1`，dynslot = `2`。 |
| 3 | **探测失败时默认固定槽位模式** | 安全失败模式：如果设备太老无法响应 `/api/install/slots`，回退到传统升级路径（也对空白设备有效，通过迁移路径）。 |
| 4 | **两种独立的写入计划，而非一个参数化计划** | 更易于测试、错误信息更清晰、维护更简单。跨模式混用是 bug 的来源。 |
| 5 | **迁移路径是第三种独立代码路径** | FoloToy → dynslot 是一次性迁移，不是常见场景。与普通升级流程分开保持清晰。 |

## 3. 版本探测

### 3.1 协议版本字段

向 `/api/install/slots` JSON 响应添加 `protocol_version`：

```json
{
  "protocol_version": 2,
  "count": 3,
  "free": 1234567,
  "archived": 0,
  "slots": [...]
}
```

**Schema：**

- `protocol_version`: `1` = 固定槽位（pre-dynslot），`2` = dynslot。
  为向后兼容省略此字段（视为 `1`）。
- 所有现有字段（`count`、`free`、`archived`、`slots`）不变。

**C 侧改动**（`main/meta_store_install.c`，`h_install_slots`）：

```c
off += snprintf(resp + off, sizeof(resp) - off,
    "{\"protocol_version\":%d,\"count\":%d,\"free\":%" PRIu32 ",...",
    META_PROTOCOL_VERSION, carve->count, free_bytes, archived_count);
```

其中 `META_PROTOCOL_VERSION` 是 `meta_carve.h` 中的编译时常量：

```c
#define META_PROTOCOL_VERSION 2  // carving 协议变更时递增
```

**JS 侧**（`install-slot/phone-install.js`，`parseSlots`）：

```js
export function parseSlots(text) {
  let d = null;
  try { d = JSON.parse(text); } catch { return null; }
  if (!d || !Number.isInteger(d.count) || d.count < 0 || d.count > 8 ||
      !Number.isFinite(d.free) || !Array.isArray(d.slots) ||
      d.slots.length !== d.count) return null;
  // 新增：可选 protocol_version（旧设备默认为 1）
  const protocolVersion = Number.isInteger(d.protocol_version)
    ? d.protocol_version : 1;
  // ... 现有槽位校验 ...
  return { count: d.count, free: d.free, slots, protocolVersion };
}
```

### 3.2 回退探测：分区表检查

如果设备不响应 `/api/install/slots`（例如仍在配对模式，或非常老的
固件），回退到直接读取分区表：

```js
// launcher-upgrade.js
// 注意:不能用 pool_0/pool_1 判定 —— 运行中的设备 live 表是 carved 表,池占位
// 已被 ota_N 槽位 + 数据条目替换;pool_* 只存在于安全表(出厂/未 carve 态)。
// 稳定判据是 store@0x35A000 条目(carve 记录 A/B 区,FIXED 集成员,两侧表都有);
// 遗留固定 3 槽表与原厂 FoloToy 表均无 store 分区。
export function isDynslotLayout(devicePartitions) {
  return devicePartitions.some(p => p.label === 'store' && p.offset === 0x35A000);
}
```

检测顺序（对应协议版本）：store 条目存在 → dynslot (2)；
否则有 ota_0/ota_1/ota_2 → fixed-slot (1)；
否则仅有 factory（无 ota_*）→ legacy FoloToy (0)；其余 unknown (-1)。

此回退仅在 slots API 不可达时使用。

## 4. 双模式写入计划

### 4.1 固定槽位模式（`protocol_version === 1`）

**与当前行为不变。** 现有 `upgradeWritePlan()` 返回四个步骤：

```js
export function upgradeWritePlan() {
  return [
    { name: "bootloader.bin",    offset: 0x0,       why: "second-stage bootloader" },
    { name: "partition-table.bin", offset: 0x8000,  why: "layout contract" },
    { name: "FoloToy-AI-Passport.bin", offset: 0x10000, why: "factory app" },
    { name: "ota_data_initial.bin", offset: 0x7FE000, why: "reset OTA selection" },
  ];
}
```

表比较：包表必须与设备表逐字节一致（两者都是固定三槽表）。迁移路径
（`migrationErasePlan()`）处理 factory-only → 固定槽位转换。

### 4.2 Dynslot 模式（`protocol_version === 2`）

**新写入计划。** 包中的分区表是**安全表**
（pool_0/pool_1 作为数据占位符，无 carve 应用条目）。设备上的 launcher
会在升级后从 store 记录中物化 carve 表。

```js
// 在 launcher-upgrade.js 中，条件导出：
export function upgradeWritePlan(protocolVersion) {
  if (protocolVersion >= 2) {
    return [
      { name: "bootloader.bin",      offset: 0x0,        why: "second-stage bootloader" },
      { name: "partition-table.bin", offset: 0x8000,     why: "safe table (carved at boot)" },
      { name: "FoloToy-AI-Passport.bin", offset: 0x10000, why: "factory app" },
      { name: "ota_data_initial.bin", offset: 0x7FE000,  why: "reset OTA selection" },
    ];
  }
  // 固定槽位路径（不变）
  return [
    { name: "bootloader.bin",       offset: 0x0,        why: "second-stage bootloader" },
    { name: "partition-table.bin",  offset: 0x8000,     why: "layout contract" },
    { name: "FoloToy-AI-Passport.bin", offset: 0x10000, why: "factory app" },
    { name: "ota_data_initial.bin", offset: 0x7FE000,  why: "reset OTA selection" },
  ];
}
```

**表比较规则（dynslot）：** 逐字节相等不可能（设备上的 carve 表是状态
相关的）。改为**条目级**比较：FIXED 条目（nvs、phy_init、factory、
cardid、store、otadata）在两侧必须完全一致（type/subtype/offset/size）；
carved ota_N 槽位和数据记录是动态内容，不比较。包必须带池占位
（pool_0/pool_1）——防止把固定槽位包刷进 dynslot 设备。

```js
export function comparePartitionTables(deviceTable, bundleTable, protocolVersion) {
  if (protocolVersion >= 2) {
    // Dynslot: 验证包是有效的安全表（无应用分区）
    // 且受保护区域匹配。设备上的 carve 表会不同。
    const bundleParts = parsePartitionTable(bundleTable);
    const labels = new Set(bundleParts.map(p => p.label));
    if (!labels.has('pool_0') || !labels.has('pool_1')) {
      return { ok: false, reason: "bundle table is not a dynslot safe table" };
    }
    // 仅比较受保护区域
    return compareProtectedRegions(deviceTable, bundleTable);
  }
  // 固定槽位：现有逐字节比较
  return _comparePartitionTablesFixed(deviceTable, bundleTable);
}
```

### 4.3 擦除计划差异

**固定槽位模式**（`migrationErasePlan()` — 不变）：
擦除 `0x180000→0x7FE000`（来自 factory-only 设备的遗留残留）。

**Dynslot 模式**（新增）：
常规升级不需要擦除（池内容在 launcher 更新间保留）。仅在从固定槽位
首次迁移时擦除：

```js
export function upgradeErasePlan(protocolVersion, devicePartitions) {
  if (protocolVersion >= 2) {
    // Dynslot: 检查设备是否仍有固定槽位布局（迁移情况）
    const labels = new Set(devicePartitions.map(p => p.label));
    if (labels.has('ota_0') && labels.has('ota_1') && labels.has('ota_2')) {
      // 设备有固定槽位表但运行 dynslot 固件 — 迁移
      return [{ offset: 0x180000, size: 0x680000, why: "migrate fixed→dynslot pool" }];
    }
    return []; // 无需擦除
  }
  return migrationErasePlan(); // 固定槽位路径（不变）
}
```

## 5. UI 流程

### 5.1 模式检测序列

```
用户点击 "Upgrade launcher"
  │
  ├─ 1. 读取设备分区表（0x8000，0x1000 字节）
  │     → parsePartitionTable() → devicePartitions
  │
  ├─ 2. 尝试 /api/install/slots（需要 WiFi 配对会话）
  │     ├─ 成功 → parseSlots() → 从 JSON 获取 protocolVersion
  │     └─ 失败 → detectProtocolFromTable(devicePartitions)
  │
  ├─ 3. 分发到特定模式的流程
  │     ├─ protocolVersion == 2 → dynslot 升级路径
  │     ├─ protocolVersion == 1 → 固定槽位升级路径
  │     └─ protocolVersion == 0 → 遗留迁移路径
  │
  └─ 4. 在 UI 中显示模式指示器（"Upgrading launcher (dynslot mode)"）
```

### 5.2 UI 改动

**`install-slot.html`** — 添加模式指示器：

```html
<p id="upgrade-mode-indicator" class="info" style="display:none">
  Mode: <span id="upgrade-mode-text"></span>
</p>
```

**JavaScript** — 探测后设置指示器：

```js
const protocolVersion = slotsData?.protocolVersion ??
  detectProtocolFromTable(deviceTable);
const modeLabel = protocolVersion >= 2 ? "Dynslot" :
                  protocolVersion === 1 ? "Fixed-slot" : "Legacy";
$("upgrade-mode-text").textContent = modeLabel;
$("upgrade-mode-indicator").style.display = "block";
```

**i18n** — 添加字符串：

```js
upgrade_mode_dynslot: "Dynslot 模式（动态槽位）",
upgrade_mode_fixed: "固定槽位模式（3 槽）",
upgrade_mode_legacy: "遗留模式（迁移）",
```

## 6. 包格式

发布二进制（`meta-pass_v*.bin`）已在 `0x8000` 包含安全表（dynslot）
或固定表（固定槽位）。**无需更改包格式** — 相同的混合格式容器服务
两种模式。区别在于手机如何解释和验证表。

**验证门**（`tools/verify_firmware.py`）：已更新以检查 pool_0/pool_1
作为保留区域（dynslot-design.md §4.6）。

## 7. 迁移路径（遗留固定槽位 → dynslot）

固定槽位设备首次安装 dynslot launcher 的一次性迁移：

1. 表探测：设备分区有 `ota_*` 且无 `store`（`isFixedSlotLayout`）；
   包分区是 dynslot 安全表（`isDynslotSafeTable`：pool_0/pool_1 + store）。
2. 手机记录迁移提示：分区表保持不动，已装玩法保留。
3. 写入计划切换为 `migrationWritePlan()`：**只写 bootloader + factory
   app + otadata —— 刻意不写 `partition-table.bin`**，也不做任何擦除。

**为什么不写表/不擦除**（修正自本节初稿）：`meta_carve_flash_ensure()`
只在 live 表仍是 legacy 表时才从遗留布局种子 carve
（`meta_pt_equal(live, meta_pt_legacy())` → `meta_carve_seed_legacy`，
"plays untouched"）。用安全表覆盖后，首次启动会走 "fresh device" 分支
—— 已装玩法成为 carve 永不引用的孤儿字节（池显示全空，下次安装直接
覆盖）。同理 `migrationErasePlan()` 的目标是 `0x180000→0x7FE000`，那
正是已装玩法所在区域；执行它等于销毁迁移承诺保留的数据。

4. 设备启动 → `ensure()` 看到完好的 legacy 表、无 carve 记录 →
   `seed_legacy` 以相同偏移/尺寸重建 carve → 提交物化 carved 表。
   所有玩法保持可引导。

**数据安全**：`meta_carve_seed_legacy()` 在原始偏移保留每个现有应用
镜像。断电最坏情况：记录或表由同一幂等路径在下次启动重建
（design §4.7）。

## 8. 错误处理

| 场景 | 响应 |
|------|------|
| 设备不可达 | "Connect device first"（现有） |
| 协议探测失败 | 回退到分区表检查 |
| 未知协议 | 拒绝并显示 "Unsupported firmware version" |
| 包的表对检测模式无效 | "Firmware bundle incompatible with this device" |
| 迁移擦除冲突（池中有数据） | "Backup existing games first"（与现有 slot-has-data 检查相同） |

## 9. 测试计划

### 9.1 单元测试（`launcher-upgrade.js`）

```js
// tests/test_launcher_upgrade.mjs（新文件）
import { upgradeWritePlan, comparePartitionTables, detectProtocolFromTable, upgradeErasePlan } from "../install-slot/launcher-upgrade.js";

// 测试 1: 写入计划分发
assert.equal(upgradeWritePlan(1).length, 4);  // 固定
assert.equal(upgradeWritePlan(2).length, 4);  // dynslot（相同步骤，不同的表语义）

// 测试 2: 从表检测协议
const fixedTable = buildTable(["nvs", "factory", "ota_0", "ota_1", "ota_2", "otadata"]);
const dynslotTable = buildTable(["nvs", "factory", "pool_0", "pool_1", "store", "otadata"]);
const legacyTable = buildTable(["nvs", "factory", "recovery"]);

assert.equal(detectProtocolFromTable(fixedTable), 1);
assert.equal(detectProtocolFromTable(dynslotTable), 2);
assert.equal(detectProtocolFromTable(legacyTable), 0);

// 测试 3: 表比较 — 固定模式（字节相等）
// 测试 4: 表比较 — dynslot 模式（仅受保护区域）
// 测试 5: 擦除计划 — 固定槽位升级（无需擦除）
// 测试 6: 擦除计划 — dynslot 常规升级（无需擦除）
// 测试 7: 擦除计划 — 迁移 fixed→dynslot（擦除池区域）
```

### 9.2 集成测试

- USB 升级：固定槽位设备 → 固定槽位包（现有路径，绿）
- USB 升级：dynslot 设备 → dynslot 包（新路径）
- USB 升级：固定槽位设备 → dynslot 包（迁移路径）
- USB 升级：dynslot 设备 → 固定槽位包（拒绝：不兼容）

### 9.3 向后兼容

- 旧设备（无 `/api/install/slots`）继续通过分区表回退工作。
- 旧包（固定槽位表）被 dynslot 设备拒绝，显示明确错误消息。
- 新包（安全表）被固定槽位设备拒绝（表比较捕获）。

## 10. 实施顺序

1. **向 slots API 添加 `protocol_version`**（C + JS parseSlots 更新）。
2. **向 `launcher-upgrade.js` 添加 `detectProtocolFromTable()`**。
3. **参数化 `upgradeWritePlan()`**（向后兼容：默认为固定槽位）。
4. **参数化 `comparePartitionTables()`**（按协议版本）。
5. **添加 `upgradeErasePlan()`**（迁移检测）。
6. **更新 HTML UI**（模式指示器和 i18n 字符串）。
7. **添加测试**（`test_launcher_upgrade.mjs`）。
8. **真实设备冒烟测试**（固定槽位 → dynslot 升级）。

## 11. 待决问题

- **Q1**：是否添加 `/api/info` 端点返回 `protocol_version`，无需活跃
  安装会话？（当前 slots API 需要 token；info 端点可以是公开的。）
  → **答案**：不需要。slots API 在升级发起时已经在配对会话中调用。

- **Q2**：如果用户刷入混合包（固定槽位表到 dynslot 设备，或反之）会怎样？
  → **答案**：表比较会拒绝。dynslot 模式的安全表验证检查 pool_0/pool_1
  存在性。

- **Q3**：M5（数据生命周期）更改 store 格式时是否应递增协议版本？
  → **答案**：不 — M5 向后兼容（新字段，旧解析器忽略它们）。只有结构
  表变更才需要递增版本。
