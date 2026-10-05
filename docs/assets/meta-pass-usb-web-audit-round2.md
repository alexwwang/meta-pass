> [简体中文](./meta-pass-usb-web-audit-round2.zh_CN.md)

# meta-pass USB Service Web Audit Report — Round 2

**Date:** 2026-10-05  
**Scope:** Second-round deep audit covering issues not found in round 1 (H1-H3/M1-M5/L1/L5).  
**Method:** Per-file code review + boundary script verification + JS↔C contract cross-check.  
**Files:** `install-slot.html` (2546L), `dynslot-record.js` (712L), `launcher-upgrade.js` (524L), `mock-device.js` (273L), `server.mjs` (303L), `meta_carve_flash.c` (660L), `validate.sh` (262L)

> **Items fixed in round 1 are out of scope.** This report lists only new issues not found in round 1.

---

## Findings Summary

| ID | Severity | File | Issue | Effort |
|---|---|---|---|---|
| N0 | 🔴 HIGH | install-slot.html:422 | M2 fix introduced ReferenceError: `checkTable` called but not imported from `dynslot-record.js` | 2 min |
| N1 | 🔴 HIGH | dynslot-record.js:325 | data label out-of-bounds read: 16-char label without NUL terminator swallows 24 bytes | 20 min |
| N2 | 🔴 HIGH | install-slot.html:657 | removeSlotAction busy not reset after cancel confirm → page permanently locked | 5 min |
| N3 | 🟡 MED | install-slot.html:2439 | install CREATE: commitCarve succeeds but payload write fails → orphan slot + boot loop, no rollback | 30 min |
| N4 | 🟡 MED | install-slot.html:2247 | restore job.to lacks boundary validation → out-of-bounds slot access | 10 min |
| N5 | 🟡 MED | server.mjs:264-270 | /api/extracted missing rate limit (/api/analyze has it, extracted missing) | 10 min |
| N6 | 🟡 MED | server.mjs:148-157 | response missing security headers (no CSP / X-Content-Type-Options / X-Frame-Options) | 15 min |
| N7 | 🟡 MED | meta_carve_flash.c:196-229 | s_in_progress not cleared on esp_flash_read failure path (line 216) → deadlock | 5 min |
| N8 | 🟡 MED | dynslot-record.js:149,162-163 | encodeRecord/carveValid input validation lacks lower bound → negative/out-of-range seq silently truncated | 20 min |
| N9 | ⚪ LOW | install-slot.html:2126 | backup save cancel (AbortError) path busy not reset | 5 min |
| N10 | ⚪ LOW | launcher-upgrade.js:467-476 | comparePartitionTables only prefix comparison, ignores non-FF entries at tail of device table | 15 min |
| N11 | ⚪ LOW | launcher-upgrade.js:245,310 | MPUP magic compares only 6 bytes, skips trailing 2×NUL | 5 min |
| N12 | ⚪ LOW | dynslot-record.js:383 | M1 fix leftover duplicate end-of-line comment | 1 min |
| N13 | ⚪ LOW | dynslot-record.js:473-477 | planInstall(null) throws TypeError instead of returning NONE | 10 min |
| N14 | ⚪ LOW | docs/assets/meta-pass-usb-web-audit.md §2.1/§4 | Round 1 audit doc record format description all wrong (256B/MPCK/uint16 → actual 4096B/MPSC/u32) | 15 min |
| N15 | ⚪ LOW | meta_carve_flash.c:318 | L5 fix still has one leftover blank line | 1 min |
| N16 | 🟡 MED | install-slot.html catch blocks | catch blocks of four operation flows (upgrade/backup/install/restore) do not reset progress bar | 10 min |
| N17 | 🟡 MED | install-slot.html:1467 | markDisconnected() does not reset busy flag and progress bar → UI state inconsistency window after disconnect | 10 min |
| N18 | 🟡 MED | install-slot.html:2335 | restoreZip not cleared on restore failure → subsequent operations may use stale data | 5 min |
| N19 | ⚪ LOW | install-slot.html:257 | launcher-upgrade.js import statement has abnormal indentation (2-space prefix) | 1 min |
| N20 | ⚪ LOW | install-slot.html:1214 | applyLang data-i18n elements still use innerHTML (L1 fix did not cover this) | 5 min |

---

## Detailed Findings

### N1 🔴 HIGH: data label decode out-of-bounds read — 16-char label causes record rejection

**Location:** `install-slot/dynslot-record.js:325` (decodeRecord data loop) + `:131-138` (labelPrintable) + `readName:264-272`

**Issue:**  
`decodeRecord` decodes the `label` field of data entries by calling `readName(raw, p + 16)`，`readName` scans `NAME_MAX=40` bytes until NUL. But the label field of a data record is only **16 bytes** (= `LABEL_MAX`). When the label is exactly 16 chars (legal on C side, JS `encodeRecord` can also produce it) and has no NUL terminator, `readName` swallows the following 24 bytes (0xFF or bytes of the next data record), causing `labelPrintable` to fail (`> 0x7e`) and `decodeAndValidate` to reject the whole record.

**Evidence:**
```javascript
// dynslot-record.js:325 — data label decode
label: readName(raw, p + 16),   // readName scans 40B, but label field is only 16B

// dynslot-record.js:264-272 — readName impl
function readName(raw, off) {
  let s = "";
  for (let i = 0; i < NAME_MAX; i++) {   // NAME_MAX=40, OOB by 24B
    const c = raw[off + i];
    if (c === 0) break;
    s += String.fromCharCode(c);
  }
  return s;
}
```

C-side comparison（`meta_carve_store.c:214-215`）：
```c
memcpy(d->label, p+16, 16);   // exactly 16B copy
d->label[16] = '\0';            // force terminator
```

C-side `meta_carve.c` validate: `while (len <= 16 && label[len]) len++` — accepts `len==16`.

**Impact:**  
A device-side legal 16-char data label record (producible by phone/device flow) on the USB page `pickRecord → decodeAndValidate → null` → misjudged as "no record" → `DYN_FRESH` + empty carve → pool shows all empty, next install first-fit overwrites existing slot data, `commitCarve` materialized new table drops old `ota_N` entries → installed play orphaned. Device-side C decodes the same record correctly; divergence only on page side.

**Recommendation:**  
Add `readNameBounded(raw, off, maxLen)` to read only the specified byte count. Use `readNameBounded(raw, p+16, LABEL_MAX)` (16B) for data label; keep `readName(raw, p+nameOff)` (40B) for slot name — its field is exactly 40B at entry tail, no overflow. Add 15/16-char label round-trip tests.

---

### N2 🔴 HIGH: removeSlotAction busy not reset after cancel confirm

**Location:** `install-slot/install-slot.html:652-657`

**Issue:**  
M3 fix moved `busy = true` before `window.confirm` (line 652), but the early return when user clicks "Cancel" (line 657) **skips the `finally` block** (line 678 `busy = false`), because `return` is outside the `try` block.

**Evidence:**
```javascript
async function removeSlotAction(index) {
  if (busy || !loader) return;
  // ...
  busy = true;                    // line 652 — set busy
  refreshInstallButton();
  const ok = window.confirm(...);
  if (!ok) {
    log(t("msg_remove_cancelled"), "warn");
    return;                       // line 657 — busy still true!
  }
  try {                           // try block starts here
    // ...
  } catch (err) {
    // ...
  } finally {
    busy = false;                 // line 679 — cancel path never reaches
    refreshInstallButton();
  }
}
```

**Impact:**  
After user clicks "Cancel" in the delete confirm dialog, `busy` stays `true` permanently, `refreshInstallButton()` is never called again, and all buttons (install/backup/restore/upgrade/probe) are permanently disabled. User must reconnect device or refresh page to recover.

**Recommendation:**  
Move `if (!ok) { ... return; }` inside the `try` block, or explicitly reset on the cancel path:
```javascript
if (!ok) {
  log(t("msg_remove_cancelled"), "warn");
  busy = false;
  refreshInstallButton();
  return;
}
```

---

### N3 🟡 MEDIUM: install CREATE path has no rollback — commitCarve success + payload write failure causes orphan slot

**Location:** `install-slot/install-slot.html:2435-2462`

**Issue:**  
install CREATE flow has two steps:
1. `commitCarve(nextCarve, { materialize: true })` (line 2439) — writes carve record + materializes partition table, slot occupied as EMPTY/RESERVED
2. `await loader.writeFlash({ payload })` (line 2455) — writes firmware image

If step 1 succeeds and step 2 fails (USB disconnect, write error), the carve record is persisted but the slot has no valid image. Device scans `ota_N` on boot → finds empty slot → boot loop or orphan data. Code has no rollback logic (does not undo slot allocation in carve record).

**Evidence:**
```javascript
// line 2435-2442: step 1 — geometry step
if (plan && plan.action === PLAN.CREATE) {
  const nextCarve = insertSlot(slotModel.carve, plan.index, { ... });
  const info = await commitCarve(nextCarve, { materialize: true });  // persisted
  // ...
}
// line 2455: step 2 — payload write
await loader.writeFlash({  // if this fails, slot already allocated in carve record
  fileArray: [{ data: u8ToBinaryString(image.data), address }],
  // ...
});
```

**Impact:**  
Slot exists in partition table but has no image → device boot loop. `slotModel` already updated (line 2425-2428), but carve record and materialized table are irreversibly written to flash. Next install can overwrite and fix, but user may not retry (may give up after seeing error).

**Recommendation:**  
Two layers of defense:
1. **UI layer**: after install failure, warn user "slot allocated but image not written — reinstall or delete this slot"
2. **(Optional) rollback layer**: on step 2 failure, call `planRemove` + `commitCarve` to roll back slot allocation. But this introduces extra flash writes; after weighing, UI warning is safer.

---

### N4 🟡 MEDIUM: restore job.to lacks boundary validation

**Location:** `install-slot/install-slot.html:2245-2253`

**Issue:**  
restore flow reads target slot number from `<select>` element `job.to = Number($(`restore-target-${s.slot}`).value)` but does not validate whether `job.to` is within `deviceSlots` range. If `<select>` is maliciously modified (DOM injection) or `deviceSlots` changes during restore (different slot count after device reconnect), `deviceSlots[job.to]` returns `undefined`, and subsequent `s.size` / `s.offset` throws TypeError or write address is `undefined`.

**Evidence:**
```javascript
const jobs = restoreZip.manifest.slots.map((s) => ({
  from: s.slot,
  to: Number($(`restore-target-${s.slot}`).value),  // no validation
}));
// ...
for (const job of jobs) {
  const s = deviceSlots[job.to];   // may be undefined
  const fit = checkRestoreFit(files, s.size);   // TypeError: Cannot read 'size' of undefined
```

**Impact:**  
Theoretically requires DOM tampering to trigger; actual risk is low but represents defensive programming gap.

**Recommendation:**  
```javascript
if (!Number.isInteger(job.to) || job.to < 0 || job.to >= deviceSlots.length) {
  throw new Error(t("err_restore_target_invalid", { slot: job.to }));
}
```

---

### N5 🟡 MEDIUM: /api/extracted missing rate limit

**Location:** `tools/install-slot/server.mjs:229-275`

**Issue:**  
Both `/api/analyze` and `/api/extracted` annotate rate-limit intent (line 192-193, 226-228), but `/api/extracted` handler lacks `rateLimited()` call. `/api/analyze` calls it at line 196; `/api/extracted` jumps straight to parameter validation at line 229-234.

**Evidence:**
```javascript
// /api/analyze (line 195-199) — has rate limit
if (pathname === "/api/analyze") {
  if (rateLimited(res.socket.remoteAddress ?? "?")) {
    sendError(res, 429, "rate limited");
    return;
  }
  // ...

// /api/extracted (line 229-234) — missing rate limit
if (pathname === "/api/extracted") {
  // no rateLimited() call!
  const id = urlObj.searchParams.get("id");
  // ...
```

**Impact:**  
`/api/extracted` returns complete firmware image (can be hundreds of KB); without rate limit it can be abused to exhaust bandwidth/backend resources. `/api/analyze` is rate-limited but extracted was missed — attacker can bypass analyze and hit extracted directly.

**Recommendation:**  
Add `rateLimited()` check at the start of `/api/extracted` handler, symmetric with `/api/analyze`.

---

### N6 🟡 MEDIUM: server.mjs response missing security headers

**Location:** `tools/install-slot/server.mjs:148-157, 164-169`

**Issue:**  
HTML and static file responses do not set security headers:
- No `Content-Security-Policy` (allows inline script/external resource injection)
- No `X-Content-Type-Options: nosniff` (MIME sniffing risk)
- No `X-Frame-Options` (clickjacking)

**Evidence:**
```javascript
// HTML response (line 148-153)
res.writeHead(200, {
  "content-type": "text/html; charset=utf-8",
  "cache-control": "no-store",
  // no CSP / X-Content-Type-Options / X-Frame-Options
});

// static file response (line 164-167)
res.writeHead(200, {
  "content-type": staticEntry.type,
  "cache-control": "no-store",
  // same as above
});
```

**Impact:**  
Local dev server risk is low (localhost), but if deployed to production (Cloudflare Pages), missing CSP means any XSS (e.g. residual innerHTML) can load external scripts. Missing `X-Content-Type-Options` allows browser to sniff MIME type.

**Recommendation:**  
At minimum add:
```
"content-security-policy": "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'",
"x-content-type-options": "nosniff",
"x-frame-options": "DENY"
```

---

### N7 🟡 MEDIUM: meta_carve_flash_table_write s_in_progress not cleared on read-back failure path

**Location:** `main/meta_carve_flash.c:196-229`

**Issue:**  
M5 reentrancy guard `s_in_progress` is set to `true` at line 198, but on the `esp_flash_read` failure path at line 216 (return ESP_FAIL at line 219) and the `memcmp` failure path at line 217 (return ESP_FAIL at line 219) **`s_in_progress` is not cleared**. Only the success path (line 229) clears it.

**Evidence:**
```c
static bool s_in_progress = false;
if (s_in_progress) return ESP_FAIL;
s_in_progress = true;
// ...
chip->os_func = saved_os;          // line 210 — restore os_func (executed on all paths)
esp_err_t e = esp_flash_erase_region(...);  // line 205
if (e == ESP_OK) {
  e = esp_flash_write(...);                 // line 208
}
chip->os_func = saved_os;                   // line 210 — restore
if (e != ESP_OK) {
  return ESP_FAIL;                          // line 213 — s_in_progress not cleared!
}
if (esp_flash_read(NULL, s_live, ...) != ESP_OK ||
    memcmp(s_live, table, META_PT_SIZE) != 0) {
  return ESP_FAIL;                          // line 219 — s_in_progress not cleared!
}
esp_partition_unload_all();
s_in_progress = false;                      // line 229 — only success path clears
return ESP_OK;
```

**Impact:**  
After any erase/write/read-back failure, `s_in_progress` stays `true` permanently, and all subsequent `meta_carve_flash_table_write` calls return `ESP_FAIL` directly. Device enters "unable to materialize partition table" state; all install/delete operations fail. Requires device reboot to recover.

**Recommendation:**  
Move `s_in_progress = false` to a unified exit path at function end, or clear before both early returns:
```c
if (e != ESP_OK) {
  ESP_LOGE(TAG, "table sector erase/write failed: %s", esp_err_to_name(e));
  s_in_progress = false;   // clear guard
  return ESP_FAIL;
}
if (esp_flash_read(...) != ESP_OK || memcmp(...) != 0) {
  ESP_LOGE(TAG, "table read-back mismatch");
  s_in_progress = false;   // clear guard
  return ESP_FAIL;
}
```

---

### N8 🟡 MEDIUM: encodeRecord/carveValid input validation lacks lower bound

**Location:** `install-slot/dynslot-record.js:149,162-163,404-416`

**Issue:**  
`carveValid` only checks upper bound for `state`/`kind`/`playId`/`subtype` (`> SLOT_STATE.INVALID`) without lower bound (negatives). `encodeRecord` only does `>>> 0` truncation for `seq` without range validation. Negative `seq=-1` truncates to `0xFFFFFFFF` (= `SEQ_INVALID`), rejected by its own decode after writing.

**Evidence:**
```javascript
// carveValid line 149
if (s.kind > SLOT_KIND.STORAGE || s.state > SLOT_STATE.INVALID) return false;
// negative -1 > 2 is false, passes validation

// encodeRecord seq handling (indirectly via commitCarve line 396-397)
let seq = (slotModel.seq + 1) >>> 0;   // -1 >>> 0 = 0xFFFFFFFF
if (seq === 0 || seq === 0xffffffff) seq = 1;   // fallback, but caller can pass any value
```

**Impact:**  
Current page call chain does not trigger (seq self-calculated, state/kind use constants), but the module serves as shared decision source (shared with Node tests); any future caller passing unvalidated data will silently produce record sectors undecodable on both ends.

**Recommendation:**  
Add `Number.isInteger(...) && x >= 0` to state/kind/playId/subtype in `carveValid`; add `seq >= 1 && seq <= 0xFFFFFFFE` validation at the top of `encodeRecord`.

---

### N9 ⚪ LOW: backup save cancel (AbortError) path busy not reset

**Location:** `install-slot/install-slot.html:2122-2127`

**Issue:**  
backup flow executes `return` (line 2126) when `showSaveFilePicker` throws `AbortError` (user cancels save), but at this point inside the `try` block, `finally` (line 2148) `busy = false` will execute. **Actually not a bug** — `return` inside `try` triggers `finally`. But `setBackupProgress(100)` is not called, leaving progress bar residue.

**Evidence:**
```javascript
if (err?.name === "AbortError") {
  log(...);
  $("backup-status").textContent = t("msg_backup_save_cancelled");
  $("backup-status").className = "error";
  return;   // return inside try → finally executes (busy correctly reset)
}
```

**Update:** After re-review, busy reset is correct (`finally` executes). Only residue is progress bar not reset. Downgraded to LOW.

**Recommendation:** Add `setBackupProgress(0)` before AbortError return.

---

### N10 ⚪ LOW: comparePartitionTables prefix-only comparison

**Location:** `install-slot/launcher-upgrade.js:467-476`

**Issue:**  
Fixed-slot path only does byte-by-byte comparison of "bundle length prefix"; device table content after `bundleTable.length` (non-0xFF extra entries) is completely ignored.

**Recommendation:** When bundle is shorter than 0x1000, require device table `[bundleTable.length, PARTITION_TABLE_READ_SIZE)` to be all 0xFF, otherwise reject.

---

### N11 ⚪ LOW: MPUP magic compares only 6 bytes

**Location:** `install-slot/launcher-upgrade.js:245,310`

**Issue:**  
`MPUPV1`/`MPUPV2` magic is defined as 8 bytes (including 2×NUL terminator), but comparison only takes the first 6 bytes.

**Recommendation:** Compare full 8 bytes.

---

### N12 ⚪ LOW: M1 fix leftover duplicate comment

**Location:** `install-slot/dynslot-record.js:383`

**Issue:**  
```javascript
fromA = ((ra.seq - rb.seq) | 0) > 0;   // signed-diff wraparound compare (explicit parens)   // signed-diff wraparound compare
```
End-of-line comment is duplicated.

**Recommendation:** Delete the duplicate fragment.

---

### N13 ⚪ LOW: planInstall(null) throws TypeError

**Location:** `install-slot/dynslot-record.js:473-477`

**Issue:**  
`planInstall(null, ...)` throws TypeError at `carve.slots[index]` instead of returning `NONE`. Current page always passes non-null carve (`loadSlotModel` falls back to `emptyCarve`), unreachable.

**Recommendation:** Add `carve = carve ?? emptyCarve()` at the start of `planInstall`.

---

### N14 ⚪ LOW: Round 1 audit doc §2.1/§4 record format description all wrong

**Location:** `docs/assets/meta-pass-usb-web-audit.md:34-62, 402-418`

**Issue:**  
Round 1 audit doc §2.1 describes carve record format completely contradicting actual code:

| Field | Doc Description | Actual Code |
|---|---|---|
| Record size | 256B | 4096B (`REC_SIZE`) |
| Magic | MPCK (0x4D50434B) | MPSC (0x4353504D); MPCK is credential backup magic |
| STORE_OFFSET | 0x8000 | 0x35A000 |
| Slot entry size | 20B | 92B (`REC_SLOT_SIZE`) |
| offset field | uint16 (2B) | uint32 (4B, `rdU32`) |
| CRC offset | 0xA0 | 4080 (`REC_CRC_OFF`) |
| Alignment invariant | `offset % 0x2000` | `offset % 0x10000` (`OFFSET_ALIGN`) |

Contract matrix marked "✓ Match" line by line is based on wrong data.

**Recommendation:** Rewrite §2.1/§4 per `meta_carve_store.h`, or directly reference header `_Static_assert` values.

---

### N15 ⚪ LOW: L5 fix still has leftover blank line

**Location:** `main/meta_carve_flash.c:318`

**Issue:**  
L5 fix removed extra blank line in `meta_carve_flash_commit`, but line 318 (after `s_reboot_pending = true; }`) still has one blank line.

**Recommendation:** Delete the blank line at line 318.

---

## Risk Matrix

| ID | Risk Type | Trigger | Consequence |
|---|---|---|---|
| N1 | Data loss | Device has 16-char data label record | USB page misjudges no record → install overwrites existing slot |
| N2 | Functional lock | User clicks "Cancel" at delete confirm | Page permanently busy, all buttons disabled |
| N3 | boot loop | install CREATE: commitCarve success + payload write failure | Orphan slot, device boot loop |
| N7 | Functional lock | table_write any failure | Device permanently unable to materialize partition table |
| N5 | Resource exhaustion | Attacker repeatedly requests /api/extracted | Bandwidth/backend exhaustion |

---

## Recommendations Priority

| Priority | IDs | Total Effort |
|---|---|---|
| P0 (fix immediately) | N2, N7 | 10 min |
| P1 (fix soon) | N1, N3, N5, N6, N8 | 95 min |
| P2 (fix when idle) | N4, N9-N15 | 65 min |

---

*Audit completed: 2026-10-05*  
*Reviewed by: OMP agent (second-round deep code review + boundary verification + JS↔C contract cross-check)*

---

## Supplementary Findings (from ScoutInstallSlotHTML)

### N0 🔴 HIGH (CRITICAL): M2 fix introduced ReferenceError — checkTable not imported

**Location:** `install-slot/install-slot.html:422` (call site) + `:278-284` (import block)

**Issue:**  
M2 fix replaced partition table read-back validation from `bytesEqual(tblBack, table)` to `checkTable(tblBack)` in `commitCarve`, but **did not add `checkTable` to the import statement**. `checkTable` has `export` in `dynslot-record.js:235`, but the import block of `install-slot.html` does not include it, and there is no local definition.

**Evidence:**
```javascript
// install-slot.html:278-284 — import block (no checkTable)
import {
  MODE, PLAN, SLOT_STATE, SLOT_KIND,
  STORE_OFFSET, STORE_SECTOR_SIZE, REC_SIZE, PT_SIZE,
  emptyCarve, carveValid, pickRecord, decodeAndValidate,
  encodeRecord, materializeTable, insertSlot,
  planInstall, planRemove, detectSlotMode, isDynMode, buildSlotView,
} from "./dynslot-record.js";

// install-slot.html:422 — call site (ReferenceError!)
if (!checkTable(tblBack)) throw new Error(t("err_table_readback"));
```

`checkTable` appears only once in the entire `install-slot.html` (line 422) — not in the import list, and no local definition.

**Impact:**  
`commitCarve` throws `ReferenceError: checkTable is not defined` on the `materialize=true` path (required path for install CREATE and delete operations). This means:
- **All new slot operations (install CREATE) will necessarily fail at the partition table read-back validation step**
- **All delete slot operations will necessarily fail at the partition table read-back validation step**
- Only `materialize=false` payload-only commitCarve (VALID marker at end of install flow) is unaffected

**Why tests did not catch:** `test-wallet-web-failures.mjs` M2 test only does source text matching (checks for `checkTable` string), does not actually execute `commitCarve` function. `validate.sh` Node tests test `dynslot-record.js` module itself (`checkTable` is a local function there), not `install-slot.html` import completeness. This is the most severe consequence of "text matching ≠ runtime behavior verification".

**Recommendation:**  
Add `checkTable` to the import block:
```javascript
import {
  ..., buildSlotView, checkTable,
} from "./dynslot-record.js";
```

**This is the most severe issue found in round 2 audit — M2 fix itself broke commitCarve materialize path.**

---

### N16 🟡 MEDIUM: catch blocks of four operation flows do not reset progress bar

**Location:** `install-slot.html` — catch blocks of upgrade/backup/install/restore

**Issue:**  
catch blocks of the four operations set error status text, but none call the corresponding `setXxxProgress(0)`:
- Upgrade catch (~line 1457): does not call `setUpgradeProgress(0)`
- Backup catch (~line 2144): does not call `setBackupProgress(0)`
- Install catch (~line 2515): does not call `setProgress(0)`
- Restore catch (~line 2331): does not call `setRestoreProgress(0)`

**Impact:** After operation failure, progress bar stays at the intermediate percentage where the error occurred, misleading the user.

**Recommendation:** Add progress bar reset call at the start of each catch block.

---

### N17 🟡 MEDIUM: markDisconnected() does not reset busy and progress bars

**Location:** `install-slot.html:1467-1474`

**Issue:**  
`markDisconnected()` only resets `loader`/`transport` and button states, but does not reset `busy` flag and progress bars. When device disconnects mid-operation, `busy` is still `true` (although operation promise eventually rejects and enters finally reset, there is a several-second window).

**Recommendation:** Add `busy = false` in `markDisconnected()` and reset all progress bars.

---

### N18 🟡 MEDIUM: restoreZip not cleared on restore failure

**Location:** `install-slot.html:2335-2337`

**Issue:**  
restore finally block only resets `busy` and `deviceNvs`, not `restoreZip`. If restore fails mid-write, `restoreZip` still holds the last parsed zip data.

**Recommendation:** Add `restoreZip = null` in restore finally block.

---

### N19 ⚪ LOW: launcher-upgrade.js import statement abnormal indentation

**Location:** `install-slot.html:257`

**Issue:** import statement has 2-space prefix, inconsistent with other imports.

**Recommendation:** Remove leading space.

---

### N20 ⚪ LOW: applyLang data-i18n elements still use innerHTML

**Location:** `install-slot.html:1214`

**Issue:** L1 fix did not cover this. `applyLang` still uses `el.innerHTML = v`. Although i18n dictionary content is hardcoded controllable strings (very low risk), it violates XSS defense principle.

**Recommendation:** Change to `el.textContent = v`; handle copy containing HTML tags separately.

---

## Supplementary Findings from ScoutServerMockC

### N21 🔴 HIGH: s_reboot_pending never explicitly cleared

**Location:** `main/meta_carve_flash.c:24, 316` + `main/main.c` goto_page

**Issue:**  
`s_reboot_pending` is set to `true` on the `commit` materialize path (line 316), but **no location in code explicitly clears it**. After `goto_page()` triggers `esp_restart()`, it relies on BSS reset to zero; if reset is not triggered (`materialize=false` path), the flag persists permanently.

**Impact:**  
- User uses device after install without triggering reset → next operation may misjudge that reboot is needed
- State machine inconsistency after multiple installs

**Recommendation:**  
Explicitly clear before `esp_restart()` in `goto_page()`, or clear at the entry of `meta_carve_flash_ensure()`.

---

### N22 🟡 MEDIUM: mock-device writeFlash sector erase semantics deviate from real stub

**Location:** `install-slot/mock-device.js:216-238`

**Issue:**  
mock `writeFlash` erases 4KB sectors by data coverage range, but does not handle cross-sector write boundary cases, and ignores `flashSize/flashMode/flashFreq/compress` parameters.

**Impact:**  
mock tests pass but real device may fail due to compression/alignment issues.

---

### N23 🟡 MEDIUM: /api/plays proxy has no parameter validation + /api/firmware only prefix check

**Location:** `tools/install-slot/server.mjs:176-179, 278-285`

**Issue:**  
`/api/plays` directly passes through query string without filtering; `/api/firmware` only checks `startsWith("/api/download/")` without validating subsequent path structure.

**Impact:**  
SSRF defense depth insufficient (current BACKEND hardcoded to ai-passport.folotoy.cn, low risk).
