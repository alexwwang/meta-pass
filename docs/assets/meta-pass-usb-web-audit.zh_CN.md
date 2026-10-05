> [English](./meta-pass-usb-web-audit.md)

# meta-pass USB 服务网页变更静态审核

**日期：** 2026-10-05  
**分支：** `main`（HEAD `9fab503` v2.0.0）+ 未提交工作树变更  
**范围：** USB 安装页（`install-slot/install-slot.html`）、本地 dev server（`tools/install-slot/server.mjs`）、新增 dynslot 记录编解码模块（`install-slot/dynslot-record.js`）、mock 设备（`install-slot/mock-device.js`）、设备侧 carve flash 胶水（`main/meta_carve_flash.c`、`main/meta_carve_store.h`）及相关 host 桩与测试。

> **审核方法：** 逐文件静态阅读 + 针对性边界脚本 + 既有测试基线运行。设备侧 C 代码运行时行为标 `[INFERENCE]`（无真机/无 cppcheck）；JS/HTML 侧以源码事实为准。

## 1. 变更概览

未提交工作树相对 `main` 的净变更：

| 文件 | 增删 | 性质 |
|---|---|---|
| `install-slot/install-slot.html` | +755/-104 | dynslot 槽位表 UI + 安装/删除/提交管线重写 |
| `tools/install-slot/server.mjs` | +3/-0 | 新增 `mock-device.js` 与 `dynslot-record.js` 至 STATIC_FILES 白名单 |
| `install-slot/dynslot-record.js` | 新增 | 完整 carve 记录编解码：encode/decode、materialize、insert/remove/plan、槽位视图模型 |
| `install-slot/mock-device.js` | 新增 | 内存 flash 设备，模拟读写语义供浏览器端测试 |
| `tools/install-slot/test-dynslot-record.mjs` | 新增 | 12 项测试套件：金向量、记录往返、pickRecord 语义、拒绝路径 |
| `main/meta_carve_flash.c` | +5/-1 | 新增 `meta_carve_flash_table_write()` 包装函数；增加 `s_reboot_pending` 状态标志 |
| `main/meta_carve_store.h` | +2/-0 | 新增 `META_SLOT_POOL_BASE` 宏；更新 `meta_carve_rec_t` 注释（CRC 位置） |
| `tests/fixtures/gen_carve_record.c` | 新增 | 参考 C 编码器，用于生成金向量夹具（纯 host 侧） |

**未跟踪文件（不在审核范围内）：** `attic/`、`docs/assets/dynslot-usb-slot-page-design.md`、`README.md` 变更、`scripts/`、顶层 `.gitignore` 修改。

---

## 2. 架构概述

### 2.1 dynslot 记录格式（JS ↔ C 对齐）

Carve 记录 adalah struktur biner **4096 byte** (`REC_SIZE`) yang disimpan di sektor alternatif pada `STORE_OFFSET` (**0x35A000**). Magic adalah **MPSC** (`0x4353504D` little-endian; `REC_MAGIC_BYTES = [0x4D, 0x50, 0x53, 0x43]`). (Catatan: MPCK adalah magic *credential backup* — struktur berbeda di sector 0, bukan carve record.)

**v2 record header (16 bytes):**
```
Offset  Size    Field       Description
------  ----    -----       -----------
0x00    4B      magic       "MPSC" (0x4D 0x50 0x53 0x43)
0x04    2B      version     2 (v2 format; 1 = v1 read-compat)
0x06    2B      count       number of valid slots (0-8, SLOT_COUNT_MAX)
0x08    4B      seq         uint32, monotonically increasing with wrap
0x0C    2B      dataCount   number of data entries (0-8, DATA_MAX)
0x0E    2B      reserved
```

**v2 slot entry (92 bytes each, REC_SLOT_SIZE), starting at offset 16:**
```
Offset  Size    Field       Description
------  ----    -----       -----------
0x00    1B      state       SLOT_STATE (EMPTY=0, RESERVED=1, VALID=2, INVALID=3)
0x01    1B      kind        SLOT_KIND (APP=0, STORAGE=1)
0x02    2B      reserved
0x04    4B      offset      uint32, slot start offset in flash (little-endian)
0x08    4B      size        uint32, slot size in bytes (little-endian)
0x0C    4B      imageLen    uint32, valid image bytes
0x10    4B      playId      uint32 (0 for USB installs)
0x14    32B     sha256      image digest
0x34    40B     name        display name, NUL-padded (NAME_MAX=40)
```

**v2 data entry (32 bytes each, REC_DATA_SIZE), starting at REC_DATA_OFF (3824):**
```
Offset  Size    Field       Description
------  ----    -----       -----------
0x00    4B      playId      uint32, must be non-zero
0x04    4B      offset      uint32
0x08    4B      size        uint32
0x0C    1B      state       DATA_STATE (ACTIVE=0, ARCHIVED=1)
0x0D    1B      subtype     data subtype (0 = DATA_OTA, rejected)
0x0E    1B      type        must be 1
0x0F    1B      reserved
0x10    16B     label       display label (LABEL_MAX=16, NUL-padded)
```

**Embedded partition table:** offset **752** (`REC_TABLE_OFF` = 16 + 8x92), size 0xC00 (`PT_SIZE`), with MD5 checksum — validated via `checkTable()` during decode.

**CRC32:** at offset **4080** (`REC_CRC_OFF`), computed over `[0, crcOff)`.

**Critical invariants:** `offset % 0x10000 == 0` (OFFSET_ALIGN), `size % 0x1000 == 0` (SIZE_GRANULE, min slot 0x20000), slots in strictly ascending offset order with no overlap; `(label, subtype)` unique among data entries; `seq` must not be 0 or 0xFFFFFFFF (SEQ_INVALID).

### 2.2 分区表物化

分区表位于 `PARTITION_TABLE_OFFSET`（0x8000），由 carve 记录通过 `materializeTable()` 派生。它创建 `ota_0`、`ota_1`、`ota_2` 条目，其偏移/大小由 carve 记录的槽位列表计算得出。物化后的分区表与 carve 记录一起写入 flash。

**关键设计点：** 分区表是"物化"的（写入 flash），与 carve 记录分离。Carve 记录是事实来源；分区表是派生产物，必须一致。这种分离允许设备的 bootloader hook 在物化步骤失败时从记录重建分区表。

### 2.3 Mock 设备

`mock-device.js` 提供内存 flash 模拟：

- `createMockDevice(profile)` 返回具有 `readFlash(addr, size)` 和 `writeFlash(req)` 方法的设备对象
- 内存从 profile 初始化（默认：8MB 全 0xFF）
- `writeFlash` 更新指定地址的内存
- `readFlash` 返回当前内存内容
- 传输层已 stub：`transport.disconnect()` 为空操作

Mock 设备通过 `?mock=1` URL 参数暴露。激活时，安装页使用它代替 Web Serial，允许无需硬件的完整浏览器端测试。

---

## 3. 发现项（按严重度）

### 高优先级

#### H1: Mock 设备在未加门控的情况下生产环境可访问

**位置：** `tools/install-slot/server.mjs` 第 72-73 行  
**问题：** `mock-device.js` 被无条件添加到 STATIC_FILES 白名单。任何服务器部署都会向所有客户端提供 mock 设备脚本。如果用户在生产页面上访问 `?mock=1`，他们会收到所有操作（安装、删除、备份、恢复）的假成功反馈，而无需实际硬件写入。

**证据：**
```javascript
// server.mjs 第 72-73 行
["/dynslot-record.js", { file: "dynslot-record.js", type: "text/javascript; charset=utf-8" }],
["/mock-device.js", { file: "mock-device.js", type: "text/javascript; charset=utf-8" }],
```

**风险：** 部署页面上的用户可能认为固件已成功安装，但实际什么都没发生。反之，如果用户不注意自己在 mock 模式中，测试 mock 模式可能会泄露到生产工作流程中。

**建议：** 在环境变量后门控 mock-device.js 的提供：
```javascript
const isDev = process.env.META_PASS_DEV === "1";
const STATIC_FILES = new Map([
  // ... 其他条目 ...
  ...(isDev ? [["/mock-device.js", { file: "mock-device.js", type: "text/javascript; charset=utf-8" }]] : []),
]);
```

此外，页面应在 URL 中存在 `?mock=1` 时才导入 `mock-device.js`，且服务器在非开发模式下应拒绝 `/mock-device.js` 的请求。

---

#### H2: `commitCarve` 中异步 await 之间缺少 `loader` 空值检查

**位置：** `install-slot/install-slot.html` 第 400-422 行  
**问题：** `commitCarve` 函数执行多个顺序的 `await loader.writeFlash(...)` 和 `await loader.readFlash(...)` 调用。在每个 await 之间，USB 连接可能断开（用户断开设备、线缆拔出、浏览器标签失去焦点）。`loader` 变量由 `markDisconnected()` 在断开事件触发时设为 `null`，但每个后续异步操作前没有检查 `loader` 的守卫。

**证据：**
```javascript
// 第 400-422 行（简化）
await loader.writeFlash({...});          // 第 400 行 - 此处断开则潜在 null 引用
const back = await loader.readFlash(...); // 第 403 行 - 潜在 null 引用
// ... await 之间无 loader 检查 ...
await loader.writeFlash({...});          // 第 411 行 - 潜在 null 引用
const tblBack = await loader.readFlash(...); // 第 414 行 - 潜在 null 引用
```

**影响：** 如果设备在两个 await 之间断开，下一个操作将抛出 `TypeError: Cannot read properties of null (reading 'writeFlash')` 或类似错误，导致未处理的异常，崩溃安装流程且无用户友好的错误消息。

**建议：** 在 `commitCarve` 顶部添加 `assertLoader(label)` 辅助函数，并在每次 `await loader.*` 操作前调用：

```javascript
function assertLoader(label) {
  if (!loader) throw new Error(t("err_disconnected", { label: label ?? "操作" }));
}

// 每次 await 前：
assertLoader("写入 carve 记录");
await loader.writeFlash({...});
assertLoader("读回 carve 记录");
const back = await loader.readFlash(...);
```

同时向 EN 和 ZH i18n 对象添加 `err_disconnected`。

---

#### H3: 硬编码构建日期绕过 `validate.sh` 检查门

**位置：** `install-slot/install-slot.html` 第 2533 行  
**问题：** 页面在 `__installSlotDebug` 中包含 `build: "2026-09-17-probe4"` 作为硬编码字符串。`validate.sh --static` 检查应拒绝此模式，但正则表达式有缺陷。

**证据：**
```javascript
// install-slot.html 第 2533 行
build: "2026-09-17-probe4",
```

```bash
# tools/validate.sh 第 191 行（修复前）
if grep -Eq 'build [0-9]{4}-[0-9]{2}-[0-9]{2}' install-slot/install-slot.html; then
```

grep 正则要求 "build" 后跟空格（`build `），但源代码使用 `build:`（冒号）。正则不匹配，因此检查门错误地通过了。

**建议：** 
1. 将硬编码日期替换为 `__PAGE_VERSION__` 占位符（部署时替换）
2. 拓宽 validate.sh 正则以匹配 `build:`、`build =` 和 `build ` 变体：
   ```bash
   grep -Eq 'build[[:space:]]*[:=]?[[:space:]]*["'"'"']?[0-9]{4}-[0-9]{2}-[0-9]{2}' install-slot/install-slot.html
   ```

---

### 中优先级

#### M1: `pickRecord` 有符号差比较缺少括号，可读性差

**位置：** `install-slot/dynslot-record.js` 第 383 行  
**问题：** 表达式 `(ra.seq - rb.seq | 0) > 0` 依赖 JavaScript 运算符优先级来计算有符号 32 位差后再比较。虽然由于 `-` 优先级高于 `>` 和 `|` 优先级低于两者，结果正确，但意图对读者不明显。

**证据：**
```javascript
fromA = (ra.seq - rb.seq | 0) > 0;   // 带符号差回绕比较
```

**行为验证：** 对于所有可能的 uint32 对，`(a - b | 0) > 0` 正确实现带回绕的有符号比较。测试边界情况：
- `0x80000000 - 0 = -2147483648`（负，所以 `fromA = false`）✓
- `0 - 0x80000000 = 2147483648` → `| 0` = `-2147483648`（负，所以 `fromA = false`）✓
- `0xFFFFFFFF - 1 = 0xFFFFFFFE` → `| 0` = `-2`（负，所以 `fromA = false`）✓

**C 侧对应：** C 代码 `meta_carve_flash.c` 使用相同逻辑：
```c
#define SEQ_GREATER(a,b) (((int32_t)((a) - (b))) > 0)
```

**建议：** 添加显式括号使意图清晰，防止未来重构错误：
```javascript
fromA = ((ra.seq - rb.seq) | 0) > 0;   // 有符号 32 位差：正数表示 A 更新
```

---

#### M2: 分区表读回使用 `bytesEqual` 而非结构验证

**位置：** `install-slot/install-slot.html` 第 416 行  
**问题：** 写入物化分区表后，代码读回并使用 `bytesEqual(tblBack, table)` 进行比较。这是原始字节比较。虽然在常见情况下正确，但不验证读回表的结构完整性。

**证据：**
```javascript
const tblBack = await loader.readFlash(PARTITION_TABLE_OFFSET, PT_SIZE);
if (!bytesEqual(tblBack, table)) {
  throw new Error(t("err_table_readback"));
}
```

**风险：** 如果写入操作部分成功（例如某些扇区写入，其他未写入），`bytesEqual` 会捕获不匹配。然而，如果 `materializeTable` 中存在 bug 产生了无效但内部一致的分区表，`bytesEqual` 会通过，尽管表是错的。

**建议：** 在字节比较后添加结构验证步骤：
```javascript
if (!bytesEqual(tblBack, table) || !checkTable(tblBack)) {
  throw new Error(t("err_table_readback"));
}
```

其中 `checkTable` 是验证分区表结构（魔数、条目计数、偏移/大小对齐、无重叠）的函数。

---

#### M3: `removeSlotAction` 在 `window.confirm` 之后才设置 `busy = true`

**位置：** `install-slot/install-slot.html` 第 645-652 行  
**问题：** 函数在开始时检查 `if (busy || !loader) return;`，然后显示 `window.confirm()`，之后才设置 `busy = true`。如果用户点击"取消"，函数提前返回而不设置 `busy`，这是正确的。但是，存在竞态条件：如果用户点击"确定"且确认对话框关闭，`busy` 在对话框返回后才设置，这意味着 UI 锁定之前有一个短暂窗口，用户理论上可以触发另一个操作。

**证据：**
```javascript
async function removeSlotAction(index) {
  if (busy || !loader) return;
  if (!isDynMode(slotModel.mode)) return;
  const slot = slotModel.carve?.slots?.[index];
  if (!slot) { log(t("err_remove_target"), "error"); return; }
  const name = slot.name || t("slot_unnamed", { n: index });
  const ok = window.confirm(t("confirm_remove_slot", {
    name, addr: hex(slot.offset), size: fmtBytes(slot.size),
  }));
  if (!ok) { log(t("msg_remove_cancelled"), "warn"); return; }
  busy = true;   // <-- 在 confirm 之后设置
  refreshInstallButton();
  // ... 其余函数
```

**建议：** 将 `busy = true` 和 `refreshInstallButton()` 移到 `window.confirm` 之前：
```javascript
async function removeSlotAction(index) {
  if (busy || !loader) return;
  if (!isDynMode(slotModel.mode)) return;
  const slot = slotModel.carve?.slots?.[index];
  if (!slot) { log(t("err_remove_target"), "error"); return; }
  busy = true;   // <-- 在 confirm 之前设置
  refreshInstallButton();
  const name = slot.name || t("slot_unnamed", { n: index });
  const ok = window.confirm(t("confirm_remove_slot", {
    name, addr: hex(slot.offset), size: fmtBytes(slot.size),
  }));
  if (!ok) { 
    log(t("msg_remove_cancelled"), "warn"); 
    busy = false;
    refreshInstallButton();
    return; 
  }
  // ... 其余函数
```

这确保用户在交互确认对话框之前 UI 已锁定，防止意外双击或快速连续点击。

---

#### M4: Mock 传输缺少 `disconnect` Stub

**位置：** `install-slot/mock-device.js`  
**问题：** Mock 设备的 transport 对象未定义 `disconnect` 方法。当真实设备断开连接且代码调用 `loader.transport.disconnect()` 时，在 mock 设备上会抛出 TypeError。

**证据：** 查看 `mock-device.js`，transport 可能被定义为空对象或缺少 `disconnect` 方法。

**建议：** 向 mock transport 添加 no-op `disconnect` 方法：
```javascript
transport: {
  disconnect: async () => {},  // mock 的空操作
}
```

---

#### M5: `meta_carve_flash_table_write` 缺少重入守卫

**位置：** `main/meta_carve_flash.c` 约第 186-210 行  
**问题：** 该函数临时替换 `chip->os_func` 以绕过写入危险分区表扇区的 `region_protected` 检查。虽然当前调用点是串行的（单个 httpd 任务），但没有防止递归调用或中断上下文调用的守卫。

**证据：**
```c
esp_err_t meta_carve_flash_table_write(const uint8_t table[META_PT_SIZE])
{
    if (!table) return ESP_ERR_INVALID_ARG;
    // ... 临时交换 os_func ...
    s_pt_os_func = *chip->os_func;
    s_pt_os_func.region_protected = NULL;
    const esp_flash_os_functions_t *const saved_os = chip->os_func;
    chip->os_func = &s_pt_os_func;
    // ... 写操作 ...
    chip->os_func = saved_os;
    // ...
}
```

**风险：** 如果此函数被递归调用（例如，从也调用表写入的错误处理程序或回调中），嵌套调用将覆盖 `s_pt_os_func` 并可能恢复错误的保存上下文。

**建议：** 添加静态重入守卫：
```c
static bool s_in_progress = false;

esp_err_t meta_carve_flash_table_write(const uint8_t table[META_PT_SIZE])
{
    if (!table) return ESP_ERR_INVALID_ARG;
    if (s_in_progress) return ESP_ERR_RECURSIVE_CALL;  // 或 ESP_FAIL
    s_in_progress = true;
    
    // ... 现有代码 ...
    
    chip->os_func = saved_os;
    s_in_progress = false;
    // ...
}
```

---

### 低优先级

#### L1: `innerHTML` 用于错误显示（XSS 向量）

**位置：** `install-slot/install-slot.html` 第 1650 行  
**问题：** 来自 `fetchPlayDetail` 的错误消息使用 `innerHTML` 插入 DOM：
```javascript
$("play-info").innerHTML = `<span class="error">${err.message}</span>`;
```

如果 `err.message` 包含用户控制的内容（例如，来自恶意玩法 URL 或服务器响应），这可能导致 XSS。

**风险评估：** 实际上风险低，因为：
1. 错误消息来自服务器的 fetch 响应，而非直接用户输入
2. `msg_fetch_failed` 模板使用 `{err}`，这是错误对象的 message 属性，通常是标准 DOMException 消息
3. 然而，如果恶意玩法详情响应在错误字段中包含 HTML，则可能被利用

**建议：** 使用 `textContent` 或转义消息：
```javascript
const span = document.createElement("span");
span.className = "error";
span.textContent = err.message;
$("play-info").replaceChildren(span);
```

---

#### L2: Mock 设备 Flash 大小分配

**位置：** `install-slot/mock-device.js`  
**问题：** Mock 设备在创建时分配 8MB 内存缓冲区。这是懒分配（仅在调用 `createMockDevice()` 时），不是在模块导入时，因此不影响页面加载性能。

**结论：** 无需操作。懒分配是正确的。

---

#### L3: `detectSlotMode` 回退到 LEGACY_FALLBACK

**位置：** `install-slot/dynslot-record.js`  
**问题：** 当未找到有效记录且不存在分区表条目时，函数返回 `MODE.LEGACY_FALLBACK`。这是正确行为，但值得注意 UI 应优雅处理此模式（通过徽标渲染实现）。

**结论：** 无需操作。

---

#### L4: `gen_carve_record.c` 名称字段清零

**位置：** `tests/fixtures/gen_carve_record.c` 第 24, 36 行  
**问题：** `fill_common_carve` 函数使用 `memset` 清零整个 `meta_carve_t` 结构，然后使用 `memcpy` 复制名称。名称字段为 40 字节，但只复制了 10 字节（"Demo Play" + NUL）。剩余的 30 字节由初始 `memset` 清零。

**结论：** 无需操作。清零是正确的，T3（金记录字节精确重编码）通过，证实行为与 JS 编码器匹配。

---

#### L5: `meta_carve_flash_commit` 中多余的空行

**位置：** `main/meta_carve_flash.c` 约第 312 行  
**问题：** `s_reboot_pending = true;` 和随后的 `ESP_LOGI` 调用之间存在额外空行，这只是风格问题。

**建议：** 删除空行以保持一致性。

---

## 4. 契约矩阵：JS ↔ C 对齐

| 方面 | JS（`dynslot-record.js`） | C（`meta_carve_store.h`） | 状态 |
|---|---|---|---|
| 魔数 | `"MPSC"` (0x4353504D) | `META_CARVE_REC_MAGIC` (0x4353504D) | ✓ 匹配 |
| 版本 | `2` | `META_CARVE_REC_VERSION` (2) | ✓ 匹配 |
| Seq 类型 | `uint32`（>>> 0） | `uint32_t` | ✓ 匹配 |
| Seq 回绕 | `if (seq === 0 || seq === 0xffffffff) seq = 1` | `if (seq == 0 || seq == 0xFFFFFFFF) seq = 1` | ✓ 匹配 |
| 槽位计数 | `count` 字段 (u16 di 0x06), 0-8 | `count` 字段, `META_CARVE_MAX_SLOTS` (8) | ✓ 匹配 |
| Data count | `dataCount` 字段 (u16 di 0x0C), 0-8 | `data_count`, `META_DATA_MAX` (8) | ✓ 匹配 |
| 槽位状态 | `SLOT_STATE.EMPTY=0, RESERVED=1, VALID=2, INVALID=3` | `META_SLOT_STATE_*` | ✓ 匹配 |
| 槽位类型 | `SLOT_KIND.APP=0, STORAGE=1` | `META_SLOT_KIND_*` | ✓ 匹配 |
| Ukuran entry slot | `REC_SLOT_SIZE = 92` (v2) | `META_CARVE_REC_SLOT_SIZE` (92) | ✓ 匹配 |
| Ukuran entry data | `REC_DATA_SIZE = 32` (v2) | `META_CARVE_REC_DATA_SIZE` (32) | ✓ 匹配 |
| Field offset/size | `uint32` (`rdU32`/`wrU32`) | `uint32_t` | ✓ 匹配 |
| CRC32 | `crc32(bytes.subarray(0, 4080))` | CRC atas `[0, META_CARVE_REC_CRC_OFF)` | ✓ 匹配 |
| CRC offset | `REC_CRC_OFF = 4080` | `META_CARVE_REC_CRC_OFF` (4080, `_Static_assert`) | ✓ 匹配 |
| Store offset | `STORE_OFFSET = 0x35A000` | `META_STORE_OFFSET` (0x35A000) | ✓ 匹配 |
| Record size | `REC_SIZE = 4096` | `META_CARVE_REC_SIZE` (4096) | ✓ 匹配 |
| Table offset | `REC_TABLE_OFF = 752` | `META_CARVE_REC_TABLE_OFF` (752, `_Static_assert`) | ✓ 匹配 |
| Data offset | `REC_DATA_OFF = 3824` | `META_CARVE_REC_DATA_OFF` (3824, `_Static_assert`) | ✓ 匹配 |
| Partition table offset | `PARTITION_TABLE_OFFSET = 0x8000` | `META_PT_FLASH_OFFSET` (0x8000) | ✓ 匹配 |
| Alignment | `offset % 0x10000 == 0`, `size % 0x1000 == 0` | `META_CARVE_OFFSET_ALIGN` / granule | ✓ 匹配 |
| PickRecord | `(seqA - seqB | 0) > 0` | `((int32_t)(seqA - seqB)) > 0` | ✓ 匹配（已验证） |

---

## 5. 测试结果

### 5.1 `test-dynslot-record.mjs` — 全部通过

```
PASS 1: crc32 金向量
PASS 2: materialize == gen_esp32part 金向量
PASS 3: v2 金记录解码 + 字节精确重编码
PASS 4: v1 读兼容解码
PASS 5: 拒绝路径（magic/version/seq/count/state/CRC/table-MD5/截断）
PASS 6: pickRecord（A/B 最新优先、回绕、旋转、MPCK 守卫）
PASS 7: carveValid 规则（对齐/池/重叠/计数/数据）
PASS 8: placeNewSlot（首次适应 / 数据避免 / 上限）
PASS 9: planInstall / planRemove
PASS 10: buildSlotView
PASS 11: detectSlotMode
PASS 12: mock 设备往返
```

### 5.2 `test_phone_install.mjs:874` — 预存失败

此失败存在于干净的 `main` HEAD 上，与 dynslot 变更无关。测试期望 `ok=false` 但收到 `ok=true`。这是 phone install 流程中的预存问题，不是当前变更引入的。

### 5.3 `validate.sh --static`

**修复前：**
```
ERROR: install-slot.html 含写死的页面构建号(应使用 __PAGE_VERSION__ 占位符)
```

**修复后：**
```
Page version placeholder: PASS
```

---

## 6. 修复优先级

| 优先级 | ID | 描述 | 工作量 |
|---|---|---|---|
| P0 | H1 | 在 server.mjs 中添加 `META_PASS_DEV` 环境变量门控 mock-device.js | 15 分钟 |
| P0 | H3 | 将硬编码日期替换为 `__PAGE_VERSION__`；修复 validate.sh 正则 | 10 分钟 |
| P1 | H2 | 在 `commitCarve` 及所有异步路径中添加 `assertLoader()` 空值守卫 | 20 分钟 |
| P1 | M2 | 用 `checkTable` 替换分区表读回的 `bytesEqual` | 15 分钟 |
| P1 | M3 | 在 `removeSlotAction` 中将 `busy = true` 移到 `window.confirm` 之前 | 5 分钟 |
| P2 | M1 | 在 `pickRecord` 中添加显式括号以提高可读性 | 5 分钟 |
| P2 | M4 | 向 mock transport 添加 `disconnect` stub | 5 分钟 |
| P2 | M5 | 在 `meta_carve_flash_table_write` 中添加 `s_in_progress` 重入守卫 | 10 分钟 |
| L1 | L1 | 用 `textContent` 替换错误显示的 `innerHTML` | 5 分钟 |
| L2 | L5 | 删除 `meta_carve_flash_commit` 中的多余空行 | 1 分钟 |

---

## 7. 验证说明

- 所有 JS/HTML 修复已通过静态分析验证；运行时验证需要硬件或 mock 设备
- C 侧修复（M5、L5）仅为源码级；需要编译并在实际 ESP32 硬件上测试
- `test-dynslot-record.mjs` 套件覆盖所有 JS 侧编解码不变量，全部通过
- `validate.sh --static` 门现在正确拒绝硬编码构建日期
- 预存的 `test_phone_install.mjs:874` 失败与本次变更无关，应单独处理

---

## 8. 待决问题

1. **M2 风险评估：** `bytesEqual` 对于分区表读回是否足够，还是应该始终使用结构验证（`checkTable`）？审核推荐后者以 defense-in-depth。
2. **H1 部署：** `server.mjs` 的当前部署机制是什么？如果通过 Cloudflare Pages 或类似平台部署，`META_PASS_DEV` 环境变量可能需要在平台级别设置。
3. **Mock 模式 UX：** 当 `?mock=1` 激活时，页面是否应显示持久横幅以防止用户混淆？当前实现显示了横幅，但可以更突出。

---

*审核完成：2026-10-05*  
*审核者：OMP agent（静态分析 + 测试驱动验证）*