> [English](./meta-pass-usb-web-audit-round2.md)

# meta-pass USB Service Web Audit Report — Round 2（第二轮审计报告）

**Date:** 2026-10-05  
**Scope:** 第二轮深度审计，覆盖第一轮（H1-H3/M1-M5/L1/L5）之后未发现的问题。  
**Method:** 逐文件代码审查 + 边界脚本验证 + JS↔C 契约交叉比对。  
**Files:** `install-slot.html` (2546L), `dynslot-record.js` (712L), `launcher-upgrade.js` (524L), `mock-device.js` (273L), `server.mjs` (303L), `meta_carve_flash.c` (660L), `validate.sh` (262L)

> **第一轮已修复项不在此报告范围内。** 本报告仅列出第一轮未发现的新问题。

---

## Findings Summary

| ID | Severity | File | Issue | Effort |
|---|---|---|---|---|
| N0 | 🔴 HIGH | install-slot.html:422 | M2 修复引入的 ReferenceError：`checkTable` 被调用但未从 `dynslot-record.js` 导入 | 2 min |
| N1 | 🔴 HIGH | dynslot-record.js:325 | data label 越界读：16 字符标签无 NUL 终结时吞入 24 字节 | 20 min |
| N2 | 🔴 HIGH | install-slot.html:657 | removeSlotAction 取消确认后 busy 不复位 → 页面永久锁死 | 5 min |
| N3 | 🟡 MED | install-slot.html:2439 | install CREATE: commitCarve 成功后 payload 写失败 → 孤儿槽位 + boot loop，无回滚 | 30 min |
| N4 | 🟡 MED | install-slot.html:2247 | restore job.to 无边界校验 → 越界槽位访问 | 10 min |
| N5 | 🟡 MED | server.mjs:264-270 | /api/extracted 无 rate limit（/api/analyze 有，extracted 漏挂） | 10 min |
| N6 | 🟡 MED | server.mjs:148-157 | 响应缺安全头（无 CSP / X-Content-Type-Options / X-Frame-Options） | 15 min |
| N7 | 🟡 MED | meta_carve_flash.c:196-229 | s_in_progress 在 esp_flash_read 失败路径（line 216）未清除 → 死锁 | 5 min |
| N8 | 🟡 MED | dynslot-record.js:149,162-163 | encodeRecord/carveValid 输入校验无下界 → 负数/越界 seq 静默截断 | 20 min |
| N9 | ⚪ LOW | install-slot.html:2126 | backup 保存取消（AbortError）路径 busy 未复位 | 5 min |
| N10 | ⚪ LOW | launcher-upgrade.js:467-476 | comparePartitionTables 仅前缀比对，忽略设备表尾部非 FF 条目 | 15 min |
| N11 | ⚪ LOW | launcher-upgrade.js:245,310 | MPUP 魔数只比 6 字节，跳过尾部 2×NUL | 5 min |
| N12 | ⚪ LOW | dynslot-record.js:383 | M1 修复残留重复行尾注释 | 1 min |
| N13 | ⚪ LOW | dynslot-record.js:473-477 | planInstall(null) 抛 TypeError 而非返回 NONE | 10 min |
| N14 | ⚪ LOW | docs/assets/meta-pass-usb-web-audit.md §2.1/§4 | 第一轮审计文档记录格式描述全部错误（256B/MPCK/uint16 → 实际 4096B/MPSC/u32） | 15 min |
| N15 | ⚪ LOW | meta_carve_flash.c:318 | L5 修复后仍残留一处多余空行 | 1 min |
| N16 | 🟡 MED | install-slot.html catch 块 | 四个操作流程（upgrade/backup/install/restore）的 catch 块均未重置进度条 | 10 min |
| N17 | 🟡 MED | install-slot.html:1467 | markDisconnected() 未重置 busy 标志和进度条 → 断开后 UI 状态不一致窗口 | 10 min |
| N18 | 🟡 MED | install-slot.html:2335 | restore 失败时 restoreZip 未清空 → 后续操作可能使用过期数据 | 5 min |
| N19 | ⚪ LOW | install-slot.html:257 | launcher-upgrade.js import 语句有异常缩进（2 空格前缀） | 1 min |
| N20 | ⚪ LOW | install-slot.html:1214 | applyLang 中 data-i18n 元素仍用 innerHTML（L1 修复未覆盖此处） | 5 min |

---

## Detailed Findings

### N1 🔴 HIGH: data label 解码越界读 — 16 字符标签导致记录被拒

**Location:** `install-slot/dynslot-record.js:325` (decodeRecord data 循环) + `:131-138` (labelPrintable) + `readName:264-272`

**Issue:**  
`decodeRecord` 解码 data 条目的 `label` 字段时调用 `readName(raw, p + 16)`，`readName` 按 `NAME_MAX=40` 字节扫描直到 NUL。但 data 记录的 label 字段只有 **16 字节**（= `LABEL_MAX`）。当 label 恰好 16 字符（C 端合法、JS `encodeRecord` 自己也能产出）且无 NUL 终结时，`readName` 会吞入后续 24 字节（0xFF 或下一条 data 记录的字节），导致 `labelPrintable` 判定失败（`> 0x7e`），`decodeAndValidate` 整体拒收。

**Evidence:**
```javascript
// dynslot-record.js:325 — data label 解码
label: readName(raw, p + 16),   // readName 扫 40B，但 label 字段只有 16B

// dynslot-record.js:264-272 — readName 实现
function readName(raw, off) {
  let s = "";
  for (let i = 0; i < NAME_MAX; i++) {   // NAME_MAX=40，越界 24B
    const c = raw[off + i];
    if (c === 0) break;
    s += String.fromCharCode(c);
  }
  return s;
}
```

C 端对比（`meta_carve_store.c:214-215`）：
```c
memcpy(d->label, p+16, 16);   // 正好 16B 拷贝
d->label[16] = '\0';            // 强制终结
```

C 端 `meta_carve.c` validate：`while (len <= 16 && label[len]) len++` — 接受 `len==16`。

**Impact:**  
设备侧合法的 16 字符 data label 记录（手机/设备流可产生）在 USB 页 `pickRecord → decodeAndValidate → null` → 误判为「无记录」→ `DYN_FRESH` + 空 carve → 池显示全空，下一次安装 first-fit 覆盖现存槽位数据，`commitCarve` 物化的新表丢弃旧 `ota_N` 条目 → 已装玩法成孤儿。设备端 C 解码同一记录完全正常，分歧只发生在页面侧。

**Recommendation:**  
新增 `readNameBounded(raw, off, maxLen)` 仅读指定字节数。data label 用 `readNameBounded(raw, p+16, LABEL_MAX)`（16B），槽位 name 维持 `readName(raw, p+nameOff)`（40B，其字段恰好 40B 在条目尾部，无越界）。补 15/16 字符 label 往返测试。

---

### N2 🔴 HIGH: removeSlotAction 取消确认后 busy 不复位

**Location:** `install-slot/install-slot.html:652-657`

**Issue:**  
M3 修复将 `busy = true` 移到 `window.confirm` 之前（line 652），但用户点「取消」时的 early return（line 657）**跳过了 `finally` 块**（line 678 的 `busy = false`），因为 `return` 在 `try` 块外。

**Evidence:**
```javascript
async function removeSlotAction(index) {
  if (busy || !loader) return;
  // ...
  busy = true;                    // line 652 — 设置 busy
  refreshInstallButton();
  const ok = window.confirm(...);
  if (!ok) {
    log(t("msg_remove_cancelled"), "warn");
    return;                       // line 657 — busy 仍为 true！
  }
  try {                           // try 块从这里才开始
    // ...
  } catch (err) {
    // ...
  } finally {
    busy = false;                 // line 679 — 取消路径不会到达
    refreshInstallButton();
  }
}
```

**Impact:**  
用户在删除确认对话框点「取消」后，`busy` 永久为 `true`，`refreshInstallButton()` 不会再被调用，所有按钮（install/backup/restore/upgrade/probe）永久禁用。用户必须重连设备或刷新页面才能恢复。

**Recommendation:**  
将 `if (!ok) { ... return; }` 移入 `try` 块内，或在取消路径显式重置：
```javascript
if (!ok) {
  log(t("msg_remove_cancelled"), "warn");
  busy = false;
  refreshInstallButton();
  return;
}
```

---

### N3 🟡 MEDIUM: install CREATE 路径无回滚 — commitCarve 成功后 payload 写失败导致孤儿槽位

**Location:** `install-slot/install-slot.html:2435-2462`

**Issue:**  
install CREATE 流程分两步：
1. `commitCarve(nextCarve, { materialize: true })`（line 2439）— 写 carve 记录 + 物化分区表，槽位以 EMPTY/RESERVED 占位
2. `await loader.writeFlash({ payload })`（line 2455）— 写固件镜像

如果 step 1 成功、step 2 失败（USB 断开、写入错误），carve 记录已落盘但槽位无有效镜像。设备启动时扫描 `ota_N` → 发现空槽 → boot loop 或孤儿数据。代码无回滚逻辑（不会撤销 carve 记录中的槽位分配）。

**Evidence:**
```javascript
// line 2435-2442: step 1 — 几何步
if (plan && plan.action === PLAN.CREATE) {
  const nextCarve = insertSlot(slotModel.carve, plan.index, { ... });
  const info = await commitCarve(nextCarve, { materialize: true });  // 已落盘
  // ...
}
// line 2455: step 2 — payload 写
await loader.writeFlash({  // 如果这里失败，carve 记录里槽位已立
  fileArray: [{ data: u8ToBinaryString(image.data), address }],
  // ...
});
```

**Impact:**  
槽位存在于分区表但无镜像 → 设备 boot loop。`slotModel` 已更新（line 2425-2428），但 carve 记录和物化表已不可逆地写入 flash。下一次安装可以覆盖修复，但用户不一定会重试（看到错误后可能放弃）。

**Recommendation:**  
两层防御：
1. **UI 层**：install 失败后提示用户「槽位已分配但镜像未写入，请重新安装或删除该槽位」
2. **（可选）回滚层**：step 2 失败时调用 `planRemove` + `commitCarve` 回滚槽位分配。但这引入额外的 flash 写，权衡后 UI 提示更安全。

---

### N4 🟡 MEDIUM: restore job.to 无边界校验

**Location:** `install-slot/install-slot.html:2245-2253`

**Issue:**  
restore 流程从 `<select>` 元素读取目标槽位号 `job.to = Number($(`restore-target-${s.slot}`).value)`，但未校验 `job.to` 是否在 `deviceSlots` 范围内。如果 `<select>` 被恶意修改（DOM 注入）或 `deviceSlots` 在 restore 期间变化（设备重连后槽数不同），`deviceSlots[job.to]` 返回 `undefined`，后续 `s.size` / `s.offset` 抛 TypeError 或写入地址为 `undefined`。

**Evidence:**
```javascript
const jobs = restoreZip.manifest.slots.map((s) => ({
  from: s.slot,
  to: Number($(`restore-target-${s.slot}`).value),  // 无校验
}));
// ...
for (const job of jobs) {
  const s = deviceSlots[job.to];   // 可能 undefined
  const fit = checkRestoreFit(files, s.size);   // TypeError: Cannot read 'size' of undefined
```

**Impact:**  
理论上需要 DOM 篡改触发；实际风险低但属于防御性编程缺失。

**Recommendation:**  
```javascript
if (!Number.isInteger(job.to) || job.to < 0 || job.to >= deviceSlots.length) {
  throw new Error(t("err_restore_target_invalid", { slot: job.to }));
}
```

---

### N5 🟡 MEDIUM: /api/extracted 缺 rate limit

**Location:** `tools/install-slot/server.mjs:229-275`

**Issue:**  
`/api/analyze` 和 `/api/extracted` 都标注了限速意图（line 192-193, 226-228），但 `/api/extracted` handler 缺少 `rateLimited()` 调用。`/api/analyze` 在 line 196 有调用，`/api/extracted` 在 line 229-234 直接跳到参数校验。

**Evidence:**
```javascript
// /api/analyze (line 195-199) — 有限速
if (pathname === "/api/analyze") {
  if (rateLimited(res.socket.remoteAddress ?? "?")) {
    sendError(res, 429, "rate limited");
    return;
  }
  // ...

// /api/extracted (line 229-234) — 缺限速
if (pathname === "/api/extracted") {
  // 无 rateLimited() 调用！
  const id = urlObj.searchParams.get("id");
  // ...
```

**Impact:**  
`/api/extracted` 返回完整固件镜像（可数百 KB），无限速可被滥用导致带宽/后端资源耗尽。`/api/analyze` 已限速但 extracted 漏挂，攻击者可绕过 analyze 直接打 extracted。

**Recommendation:**  
在 `/api/extracted` handler 开头加 `rateLimited()` 检查，与 `/api/analyze` 对称。

---

### N6 🟡 MEDIUM: server.mjs 响应缺安全头

**Location:** `tools/install-slot/server.mjs:148-157, 164-169`

**Issue:**  
HTML 和静态文件响应未设置安全头：
- 无 `Content-Security-Policy`（允许内联脚本/外部资源注入）
- 无 `X-Content-Type-Options: nosniff`（MIME 嗅探风险）
- 无 `X-Frame-Options`（clickjacking）

**Evidence:**
```javascript
// HTML 响应 (line 148-153)
res.writeHead(200, {
  "content-type": "text/html; charset=utf-8",
  "cache-control": "no-store",
  // 无 CSP / X-Content-Type-Options / X-Frame-Options
});

// 静态文件响应 (line 164-167)
res.writeHead(200, {
  "content-type": staticEntry.type,
  "cache-control": "no-store",
  // 同上
});
```

**Impact:**  
本地 dev 服务器风险较低（localhost），但若部署到生产（Cloudflare Pages），缺少 CSP 意味着任何 XSS（如残留的 innerHTML）可加载外部脚本。`X-Content-Type-Options` 缺失允许浏览器嗅探 MIME 类型。

**Recommendation:**  
至少添加：
```
"content-security-policy": "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'",
"x-content-type-options": "nosniff",
"x-frame-options": "DENY"
```

---

### N7 🟡 MEDIUM: meta_carve_flash_table_write 的 s_in_progress 在 read-back 失败路径未清除

**Location:** `main/meta_carve_flash.c:196-229`

**Issue:**  
M5 添加的重入守卫 `s_in_progress` 在 line 198 设置为 `true`，但在 line 216 的 `esp_flash_read` 失败路径（return ESP_FAIL at line 219）和 line 217 的 `memcmp` 失败路径（return ESP_FAIL at line 219）**均未清除 `s_in_progress`**。只有成功路径（line 229）才清除。

**Evidence:**
```c
static bool s_in_progress = false;
if (s_in_progress) return ESP_FAIL;
s_in_progress = true;
// ...
chip->os_func = saved_os;          // line 210 — 恢复 os_func（在所有路径都执行）
esp_err_t e = esp_flash_erase_region(...);  // line 205
if (e == ESP_OK) {
  e = esp_flash_write(...);                 // line 208
}
chip->os_func = saved_os;                   // line 210 — 恢复
if (e != ESP_OK) {
  return ESP_FAIL;                          // line 213 — s_in_progress 未清除！
}
if (esp_flash_read(NULL, s_live, ...) != ESP_OK ||
    memcmp(s_live, table, META_PT_SIZE) != 0) {
  return ESP_FAIL;                          // line 219 — s_in_progress 未清除！
}
esp_partition_unload_all();
s_in_progress = false;                      // line 229 — 只有成功路径清除
return ESP_OK;
```

**Impact:**  
任何 erase/write/read-back 失败后，`s_in_progress` 永久为 `true`，后续所有 `meta_carve_flash_table_write` 调用直接返回 `ESP_FAIL`。设备进入「无法物化分区表」状态，所有 install/delete 操作失败。需重启设备恢复。

**Recommendation:**  
将 `s_in_progress = false` 移到函数末尾的统一退出路径，或在两个 early return 前清除：
```c
if (e != ESP_OK) {
  ESP_LOGE(TAG, "table sector erase/write failed: %s", esp_err_to_name(e));
  s_in_progress = false;   // 清除守卫
  return ESP_FAIL;
}
if (esp_flash_read(...) != ESP_OK || memcmp(...) != 0) {
  ESP_LOGE(TAG, "table read-back mismatch");
  s_in_progress = false;   // 清除守卫
  return ESP_FAIL;
}
```

---

### N8 🟡 MEDIUM: encodeRecord/carveValid 输入校验无下界

**Location:** `install-slot/dynslot-record.js:149,162-163,404-416`

**Issue:**  
`carveValid` 对 `state`/`kind`/`playId`/`subtype` 只检查上界（`> SLOT_STATE.INVALID`）不检查下界（负数）。`encodeRecord` 对 `seq` 只做 `>>> 0` 截断，不校验范围。负数 `seq=-1` 截断为 `0xFFFFFFFF`（= `SEQ_INVALID`），写出后被自己的 decode 拒收。

**Evidence:**
```javascript
// carveValid line 149
if (s.kind > SLOT_KIND.STORAGE || s.state > SLOT_STATE.INVALID) return false;
// 负数 -1 > 2 为 false，通过校验

// encodeRecord seq 处理（间接通过 commitCarve line 396-397）
let seq = (slotModel.seq + 1) >>> 0;   // -1 >>> 0 = 0xFFFFFFFF
if (seq === 0 || seq === 0xffffffff) seq = 1;   // 兜底，但调用方可传任意值
```

**Impact:**  
当前页面调用链不会触发（seq 自算、state/kind 用常量），但模块作为共享决策源（Node 测试共用），任何未来调用方传入未校验数据会静默产出双端不可解码的记录扇区。

**Recommendation:**  
`carveValid` 的 state/kind/playId/subtype 加 `Number.isInteger(...) && x >= 0`；`encodeRecord` 头部加 `seq >= 1 && seq <= 0xFFFFFFFE` 校验。

---

### N9 ⚪ LOW: backup 保存取消（AbortError）路径 busy 未复位

**Location:** `install-slot/install-slot.html:2122-2127`

**Issue:**  
backup 流程在 `showSaveFilePicker` 抛 `AbortError`（用户取消保存）时执行 `return`（line 2126），但此时在 `try` 块内，`finally`（line 2148）的 `busy = false` 会被执行。**实际不是 bug**——`return` 在 `try` 内会触发 `finally`。但 `setBackupProgress(100)` 不会被调用，进度条残留。

**Evidence:**
```javascript
if (err?.name === "AbortError") {
  log(...);
  $("backup-status").textContent = t("msg_backup_save_cancelled");
  $("backup-status").className = "error";
  return;   // try 块内 return → finally 会执行（busy 正确复位）
}
```

**Update:** 经复核，busy 复位正确（`finally` 会执行）。唯一残留是进度条未重置。降级为 LOW。

**Recommendation:** 在 AbortError return 前加 `setBackupProgress(0)`。

---

### N10 ⚪ LOW: comparePartitionTables 仅前缀比对

**Location:** `install-slot/launcher-upgrade.js:467-476`

**Issue:**  
固定槽位路径只做「bundle 长度前缀」逐字节比对，设备表在 `bundleTable.length` 之后的内容（非 0xFF 的额外条目）被完全忽略。

**Recommendation:** bundle 短于 0x1000 时，要求设备表 `[bundleTable.length, PARTITION_TABLE_READ_SIZE)` 全为 0xFF，否则拒绝。

---

### N11 ⚪ LOW: MPUP 魔数只比 6 字节

**Location:** `install-slot/launcher-upgrade.js:245,310`

**Issue:**  
`MPUPV1`/`MPUPV2` 魔数定义为 8 字节（含 2×NUL 终结），但比较时只取前 6 字节。

**Recommendation:** 改为比较完整 8 字节。

---

### N12 ⚪ LOW: M1 修复残留重复注释

**Location:** `install-slot/dynslot-record.js:383`

**Issue:**  
```javascript
fromA = ((ra.seq - rb.seq) | 0) > 0;   // 带符号差回绕比较(显式括号防歧义)   // 带符号差回绕比较
```
行尾注释重复。

**Recommendation:** 删掉重复片段。

---

### N13 ⚪ LOW: planInstall(null) 抛 TypeError

**Location:** `install-slot/dynslot-record.js:473-477`

**Issue:**  
`planInstall(null, ...)` 在 `carve.slots[index]` 处抛 TypeError 而非返回 `NONE`。当前页面恒传非空 carve（`loadSlotModel` 兜底 `emptyCarve`），不可达。

**Recommendation:** `planInstall` 开头 `carve = carve ?? emptyCarve()`。

---

### N14 ⚪ LOW: 第一轮审计文档 §2.1/§4 记录格式描述全部错误

**Location:** `docs/assets/meta-pass-usb-web-audit.md:34-62, 402-418`

**Issue:**  
第一轮审计文档 §2.1 描述的 carve 记录格式与实际代码完全矛盾：

| 字段 | 文档描述 | 实际代码 |
|---|---|---|
| 记录大小 | 256B | 4096B (`REC_SIZE`) |
| 魔数 | MPCK (0x4D50434B) | MPSC (0x4353504D)；MPCK 是凭据备份魔数 |
| STORE_OFFSET | 0x8000 | 0x35A000 |
| 槽条目大小 | 20B | 92B (`REC_SLOT_SIZE`) |
| offset 字段 | uint16 (2B) | uint32 (4B, `rdU32`) |
| CRC 偏移 | 0xA0 | 4080 (`REC_CRC_OFF`) |
| 对齐不变量 | `offset % 0x2000` | `offset % 0x10000` (`OFFSET_ALIGN`) |

文档逐行标 "✓ Match" 的契约矩阵基于错误数据。

**Recommendation:** 按 `meta_carve_store.h` 重写 §2.1/§4，或直接引用头文件 `_Static_assert` 数值。

---

### N15 ⚪ LOW: L5 修复后仍残留多余空行

**Location:** `main/meta_carve_flash.c:318`

**Issue:**  
L5 修复删除了 `meta_carve_flash_commit` 中的多余空行，但 line 318（`s_reboot_pending = true; }` 之后）仍有一处空行。

**Recommendation:** 删除 line 318 的空行。

---

## Risk Matrix

| ID | 风险类型 | 触发条件 | 后果 |
|---|---|---|---|
| N1 | 数据丢失 | 设备有 16 字符 data label 的记录 | USB 页误判无记录 → 安装覆盖现存槽位 |
| N2 | 功能锁死 | 用户在删除确认点「取消」 | 页面永久 busy，所有按钮禁用 |
| N3 | boot loop | install CREATE: commitCarve 成功 + payload 写失败 | 孤儿槽位，设备 boot loop |
| N7 | 功能锁死 | table_write 任意失败 | 设备永久无法物化分区表 |
| N5 | 资源耗尽 | 攻击者反复请求 /api/extracted | 带宽/后端耗尽 |

---

## Recommendations Priority

| Priority | IDs | Total Effort |
|---|---|---|
| P0 (立即修复) | N2, N7 | 10 min |
| P1 (近期修复) | N1, N3, N5, N6, N8 | 95 min |
| P2 (空闲修复) | N4, N9-N15 | 65 min |

---

*Audit completed: 2026-10-05*  
*Reviewed by: OMP agent (second-round deep code review + boundary verification + JS↔C contract cross-check)*

---

## Supplementary Findings (from ScoutInstallSlotHTML)

### N0 🔴 HIGH (CRITICAL): M2 修复引入 ReferenceError — checkTable 未导入

**Location:** `install-slot/install-slot.html:422`（调用处）+ `:278-284`（import 块）

**Issue:**  
M2 修复将 `commitCarve` 中分区表读回校验从 `bytesEqual(tblBack, table)` 替换为 `checkTable(tblBack)`，但**未在 import 语句中添加 `checkTable`**。`checkTable` 在 `dynslot-record.js:235` 有 `export`，但 `install-slot.html` 的 import 块不包含它，也没有本地定义。

**Evidence:**
```javascript
// install-slot.html:278-284 — import 块（无 checkTable）
import {
  MODE, PLAN, SLOT_STATE, SLOT_KIND,
  STORE_OFFSET, STORE_SECTOR_SIZE, REC_SIZE, PT_SIZE,
  emptyCarve, carveValid, pickRecord, decodeAndValidate,
  encodeRecord, materializeTable, insertSlot,
  planInstall, planRemove, detectSlotMode, isDynMode, buildSlotView,
} from "./dynslot-record.js";

// install-slot.html:422 — 调用处（ReferenceError!）
if (!checkTable(tblBack)) throw new Error(t("err_table_readback"));
```

`checkTable` 在整个 `install-slot.html` 中仅出现这一次（line 422），既不在 import 列表中，也没有本地定义。

**Impact:**  
`commitCarve` 在 `materialize=true` 路径（install CREATE 和 delete 操作的必经路径）会抛出 `ReferenceError: checkTable is not defined`。这意味着：
- **所有新建槽位操作（install CREATE）在分区表读回校验步骤必然失败**
- **所有删除槽位操作在分区表读回校验步骤必然失败**
- 只有 `materialize=false` 的 payload-only commitCarve（install 流程末尾的 VALID 标记）不受影响

**测试为什么没发现：** `test-wallet-web-failures.mjs` 对 M2 的测试只做源码文本匹配（检查有没有 `checkTable` 字样），不实际执行 `commitCarve` 函数。`validate.sh` 的 Node 测试测的是 `dynslot-record.js` 模块本身（`checkTable` 是本地函数），不是 `install-slot.html` 的 import 完整性。这是「文本匹配 ≠ 运行时行为验证」的最严重后果。

**Recommendation:**  
在 import 块中添加 `checkTable`：
```javascript
import {
  ..., buildSlotView, checkTable,
} from "./dynslot-record.js";
```

**这是第二轮审计发现的最严重问题——M2 的修复本身就破坏了 commitCarve 的 materialize 路径。**

---

### N16 🟡 MEDIUM: 四个操作流程 catch 块均未重置进度条

**Location:** `install-slot.html` — upgrade/backup/install/restore 的 catch 块

**Issue:**  
四个操作的 catch 块设置了错误状态文本，但均未调用对应的 `setXxxProgress(0)`：
- Upgrade catch (~line 1457): 未调用 `setUpgradeProgress(0)`
- Backup catch (~line 2144): 未调用 `setBackupProgress(0)`
- Install catch (~line 2515): 未调用 `setProgress(0)`
- Restore catch (~line 2331): 未调用 `setRestoreProgress(0)`

**Impact:** 操作失败后进度条停留在出错时的中间百分比，误导用户。

**Recommendation:** 在每个 catch 块开头添加进度条重置调用。

---

### N17 🟡 MEDIUM: markDisconnected() 未重置 busy 和进度条

**Location:** `install-slot.html:1467-1474`

**Issue:**  
`markDisconnected()` 只重置 `loader`/`transport` 和按钮状态，不重置 `busy` 标志和进度条。设备在操作中途断开时，`busy` 仍为 `true`（虽然操作 promise 最终会 reject 并进入 finally 重置，但存在数秒窗口期）。

**Recommendation:** 在 `markDisconnected()` 中添加 `busy = false` 并重置所有进度条。

---

### N18 🟡 MEDIUM: restore 失败时 restoreZip 未清空

**Location:** `install-slot.html:2335-2337`

**Issue:**  
restore 的 finally 块只重置了 `busy` 和 `deviceNvs`，未重置 `restoreZip`。如果 restore 在写入中途失败，`restoreZip` 仍持有上次解析的 zip 数据。

**Recommendation:** 在 restore 的 finally 块中添加 `restoreZip = null`。

---

### N19 ⚪ LOW: launcher-upgrade.js import 语句异常缩进

**Location:** `install-slot.html:257`

**Issue:** import 语句有 2 空格前缀，与其他 import 不一致。

**Recommendation:** 去除前导空格。

---

### N20 ⚪ LOW: applyLang 中 data-i18n 元素仍用 innerHTML

**Location:** `install-slot.html:1214`

**Issue:** L1 修复未覆盖此处。`applyLang` 中 `el.innerHTML = v` 仍用 innerHTML。虽然 i18n 字典内容是硬编码可控字符串（风险极低），但违反 XSS 防御原则。

**Recommendation:** 改为 `el.textContent = v`，对含 HTML 标签的文案单独处理。

---

## Supplementary Findings from ScoutServerMockC

### N21 🔴 HIGH: s_reboot_pending 从未被显式清除

**Location:** `main/meta_carve_flash.c:24, 316` + `main/main.c` goto_page

**Issue:**  
`s_reboot_pending` 在 `commit` 的 materialize 路径（line 316）被置为 `true`，但代码中**没有任何位置显式清除它**。`goto_page()` 触发 `esp_restart()` 后靠 BSS 重置归零；若未触发复位（`materialize=false` 路径），标志永久留存。

**Impact:**  
- 用户安装后未触发复位就使用设备 → 下一次操作可能误判需要重启
- 多次安装后状态机不一致

**Recommendation:**  
在 `goto_page()` 中 `esp_restart()` 前显式清零，或在 `meta_carve_flash_ensure()` 入口清零。

---

### N22 🟡 MEDIUM: mock-device writeFlash 扇区擦除语义与真实 stub 存在偏差

**Location:** `install-slot/mock-device.js:216-238`

**Issue:**  
mock 的 `writeFlash` 按数据覆盖范围擦除 4KB 扇区，但未处理跨扇区写入的边界情况，且忽略 `flashSize/flashMode/flashFreq/compress` 参数。

**Impact:**  
mock 测试通过但真机可能因压缩/对齐问题失败。

---

### N23 🟡 MEDIUM: /api/plays 代理无参数校验 + /api/firmware 仅前缀检查

**Location:** `tools/install-slot/server.mjs:176-179, 278-285`

**Issue:**  
`/api/plays` 直接透传 query string 无过滤；`/api/firmware` 仅检查 `startsWith("/api/download/")`，不验证后续路径结构。

**Impact:**  
SSRF 防御纵深不足（当前 BACKEND 硬编码为 ai-passport.folotoy.cn，风险较低）。
