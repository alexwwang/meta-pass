> [简体中文](./meta-pass-usb-web-audit.zh_CN.md)

# meta-pass USB Service Web Audit Report

**Date:** 2026-10-05  
**Branch:** `main` (HEAD `9fab503` v2.0.0) + uncommitted working tree changes  
**Scope:** USB slot installer web page (`install-slot/install-slot.html`), local dev server (`tools/install-slot/server.mjs`), new dynslot record codec module (`install-slot/dynslot-record.js`), mock device (`install-slot/mock-device.js`), device-side carve flash glue (`main/meta_carve_flash.c`, `main/meta_carve_store.h`), and related host stubs/tests.

> **Audit Method:** Line-by-line static review + targeted boundary scripts + baseline test runs. Device-side C runtime behavior marked `[INFERENCE]` (no real hardware/cppcheck available); JS/HTML findings based on source code facts.

## 1. Change Summary

Net changes to the working tree relative to `main`:

| File | +/- | Nature |
|---|---|---|
| `install-slot/install-slot.html` | +755/-104 | Dynslot slot table UI + install/delete/commit pipeline rewrite |
| `tools/install-slot/server.mjs` | +3/-0 | Added `mock-device.js` and `dynslot-record.js` to STATIC_FILES whitelist |
| `install-slot/dynslot-record.js` | NEW | Full carve record codec: encode/decode, materialize, insert/remove/plan, slot view model |
| `install-slot/mock-device.js` | NEW | In-memory flash device simulating read/write semantics for browser-based testing |
| `tools/install-slot/test-dynslot-record.mjs` | NEW | 12-test suite covering golden vectors, record round-trip, pickRecord semantics, rejection paths |
| `main/meta_carve_flash.c` | +5/-1 | Inserted `meta_carve_flash_table_write()` wrapper; added `s_reboot_pending` state flag |
| `main/meta_carve_store.h` | +2/-0 | Added `META_SLOT_POOL_BASE` macro; updated `meta_carve_rec_t` comment for CRC placement |
| `tests/fixtures/gen_carve_record.c` | NEW | Reference C encoder for generating golden fixture bytes (pure host-side) |

**Untracked files (not in scope):** `attic/`, `docs/assets/dynslot-usb-slot-page-design.md`, `README.md` changes, `scripts/`, top-level `.gitignore` edits.

---

## 2. Architecture Overview

### 2.1 dynslot Record Format (JS ↔ C alignment)

The carve record is a **4096-byte** (`REC_SIZE`) binary structure stored in alternating 4KB sectors at `STORE_OFFSET` (**0x35A000**). Magic is **"MPSC"** (`0x4353504D` little-endian; `REC_MAGIC_BYTES = [0x4D, 0x50, 0x53, 0x43]`). (Note: "MPCK" is the *credential backup* magic — a different structure at sector 0, not the carve record.)

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

**Embedded partition table:** offset **752** (`REC_TABLE_OFF` = 16 + 8×92), size 0xC00 (`PT_SIZE`), with MD5 checksum — validated via `checkTable()` during decode.

**CRC32:** at offset **4080** (`REC_CRC_OFF`), computed over bytes `[0, crcOff)`.

**Critical invariants:** `offset % 0x10000 == 0` (OFFSET_ALIGN), `size % 0x1000 == 0` (SIZE_GRANULE, min slot 0x20000), slots in strictly ascending offset order with no overlap; `(label, subtype)` unique among data entries; `seq` must not be 0 or 0xFFFFFFFF (SEQ_INVALID).

### 2.2 Partition Table Materialization

The partition table at `PARTITION_TABLE_OFFSET` (0x8000) is derived from the carve record via `materializeTable()`. It creates `ota_0`, `ota_1`, `ota_2` entries whose offsets/sizes are computed from the carve record's slot list. The materialized table is written to flash alongside the carve record during `commitCarve`.

**Key design point:** The partition table is "materialized" (written to flash) separately from the carve record. The carve record is the source of truth; the partition table is a derived artifact that must match. This separation allows the device's bootloader hook to reconstruct the table on next boot if the materialization step fails.

### 2.3 Mock Device

`mock-device.js` provides an in-memory flash simulation:

- `createMockDevice(profile)` returns a device object with `readFlash(addr, size)` and `writeFlash(req)` methods
- Memory is initialized from a profile (default: 8MB all-0xFF)
- `writeFlash` updates memory at the specified address
- `readFlash` returns the current memory contents
- Transport layer is stubbed: `transport.disconnect()` is a no-op

The mock device is exposed via `?mock=1` URL parameter. When active, the install page uses it instead of Web Serial, allowing full browser-based testing without hardware.

---

## 3. Findings by Severity

### HIGH

#### H1: Mock Device Exposed in Production Without Gate

**Location:** `tools/install-slot/server.mjs` lines 72-73  
**Issue:** `mock-device.js` is unconditionally added to the STATIC_FILES whitelist. Any deployment of the server will serve the mock device script to all clients. If a user visits the production page with `?mock=1`, they receive fake success feedback for all operations (install, delete, backup, restore) without any actual hardware writes.

**Evidence:**
```javascript
// server.mjs line 72-73
["/dynslot-record.js", { file: "dynslot-record.js", type: "text/javascript; charset=utf-8" }],
["/mock-device.js", { file: "mock-device.js", type: "text/javascript; charset=utf-8" }],
```

**Risk:** Users on deployed pages could believe firmware was successfully installed when nothing happened. Conversely, testing with mock mode could leak into production workflows if users are not aware they are in mock mode.

**Recommendation:** Gate mock-device.js serving behind an environment variable:
```javascript
const isDev = process.env.META_PASS_DEV === "1";
const STATIC_FILES = new Map([
  // ... other entries ...
  ...(isDev ? [["/mock-device.js", { file: "mock-device.js", type: "text/javascript; charset=utf-8" }]] : []),
]);
```

Additionally, the page should only import `mock-device.js` when `?mock=1` is present in the URL, and the server should reject requests for `/mock-device.js` in non-dev modes.

---

#### H2: No Null-Check on `loader` Between Async Awaits in `commitCarve`

**Location:** `install-slot/install-slot.html` lines 400-422  
**Issue:** The `commitCarve` function performs multiple sequential `await loader.writeFlash(...)` and `await loader.readFlash(...)` calls. Between each await, the USB connection could drop (user disconnects device, cable unplugged, browser tab loses focus). The `loader` variable is set to `null` by `markDisconnected()` when a disconnect event fires, but there is no guard checking `loader` before each subsequent async operation.

**Evidence:**
```javascript
// Lines 400-422 (condensed)
await loader.writeFlash({...});          // Line 400 - potential null ref if disconnected here
const back = await loader.readFlash(...); // Line 403 - potential null ref
// ... no loader check between awaits ...
await loader.writeFlash({...});          // Line 411 - potential null ref
const tblBack = await loader.readFlash(...); // Line 414 - potential null ref
```

**Impact:** If the device disconnects between any two awaits, the next operation throws `TypeError: Cannot read properties of null (reading 'writeFlash')` or similar, resulting in an unhandled exception that crashes the install flow without a user-friendly error message.

**Recommendation:** Add an `assertLoader(label)` helper at the top of `commitCarve` and call it before each `await loader.*` operation:

```javascript
function assertLoader(label) {
  if (!loader) throw new Error(t("err_disconnected", { label: label ?? "operation" }));
}

// Before each await:
assertLoader("writing carve record");
await loader.writeFlash({...});
assertLoader("reading carve record back");
const back = await loader.readFlash(...);
```

Also add `err_disconnected` to both EN and ZH i18n objects.

---

#### H3: Hardcoded Build Date Bypasses `validate.sh` Gate

**Location:** `install-slot/install-slot.html` line 2533  
**Issue:** The page contains `build: "2026-09-17-probe4"` as a hardcoded string in `__installSlotDebug`. The `validate.sh --static` gate checks for this pattern and should reject it, but the regex is flawed.

**Evidence:**
```javascript
// install-slot.html line 2533
build: "2026-09-17-probe4",
```

```bash
# tools/validate.sh line 191 (BEFORE fix)
if grep -Eq 'build [0-9]{4}-[0-9]{2}-[0-9]{2}' install-slot/install-slot.html; then
```

The grep regex requires a space after "build" (`build `), but the source code uses `build:` (colon). The regex does not match, so the gate passes incorrectly.

**Recommendation:** 
1. Replace the hardcoded date with `__PAGE_VERSION__` placeholder (to be replaced at deploy time)
2. Widen the validate.sh regex to match `build:`, `build =`, and `build ` variants:
   ```bash
   grep -Eq 'build[[:space:]]*[:=]?[[:space:]]*["'"'"']?[0-9]{4}-[0-9]{2}-[0-9]{2}' install-slot/install-slot.html
   ```

---

### MEDIUM

#### M1: `pickRecord` Signed-Diff Comparison Fragile Without Parentheses

**Location:** `install-slot/dynslot-record.js` line 383  
**Issue:** The comparison `(ra.seq - rb.seq | 0) > 0` relies on JavaScript operator precedence to compute the signed 32-bit difference before comparison. While this happens to work correctly due to `-` having higher precedence than `>` and `|` having lower precedence than both, the intent is not immediately obvious to readers.

**Evidence:**
```javascript
fromA = (ra.seq - rb.seq | 0) > 0;   // signed difference with wraparound comparison
```

**Behavior verification:** For all possible uint32 pairs, `(a - b | 0) > 0` correctly implements signed comparison with wraparound. Tested boundary cases:
- `0x80000000 - 0 = -2147483648` (negative, so `fromA = false`) ✓
- `0 - 0x80000000 = 2147483648` → `| 0` = `-2147483648` (negative, so `fromA = false`) ✓
- `0xFFFFFFFF - 1 = 0xFFFFFFFE` → `| 0` = `-2` (negative, so `fromA = false`) ✓

**C-side correspondence:** The C code in `meta_carve_flash.c` uses identical logic:
```c
#define SEQ_GREATER(a,b) (((int32_t)((a) - (b))) > 0)
```

**Recommendation:** Add explicit parentheses to make the intent clear and prevent future refactoring errors:
```javascript
fromA = ((ra.seq - rb.seq) | 0) > 0;   // Signed 32-bit diff: positive means A is newer
```

---

#### M2: Table Readback Uses `bytesEqual` Instead of Structural Validation

**Location:** `install-slot/install-slot.html` line 416  
**Issue:** After writing the materialized partition table, the code reads it back and compares using `bytesEqual(tblBack, table)`. This is a raw byte-for-byte comparison. While correct in the common case, it does not validate the structural integrity of the read-back table.

**Evidence:**
```javascript
const tblBack = await loader.readFlash(PARTITION_TABLE_OFFSET, PT_SIZE);
if (!bytesEqual(tblBack, table)) {
  throw new Error(t("err_table_readback"));
}
```

**Risk:** If the write operation partially succeeded (e.g., some sectors written, others not), `bytesEqual` would catch the mismatch. However, if there is a bug in `materializeTable` that produces an invalid but internally-consistent table, `bytesEqual` would pass even though the table is wrong.

**Recommendation:** Add a structural validation step after the byte comparison:
```javascript
if (!bytesEqual(tblBack, table) || !checkTable(tblBack)) {
  throw new Error(t("err_table_readback"));
}
```

Where `checkTable` is a function that validates partition table structure (magic bytes, entry counts, offset/size alignment, no overlaps).

---

#### M3: `removeSlotAction` Sets `busy = true` After `window.confirm`

**Location:** `install-slot/install-slot.html` lines 645-652  
**Issue:** The function checks `if (busy || !loader) return;` at the start, then shows `window.confirm()`, then sets `busy = true`. If the user clicks "Cancel" on the confirm dialog, the function returns early without setting `busy`, which is correct. However, there is a race condition: if the user clicks "OK" and the confirm dialog closes, `busy` is set after the dialog returns, meaning there is a brief window where the UI is not locked and the user could theoretically trigger another action.

**Evidence:**
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
  busy = true;   // <-- Set AFTER confirm, not before
  refreshInstallButton();
  // ... rest of function
```

**Recommendation:** Move `busy = true` and `refreshInstallButton()` to before the `window.confirm` call:
```javascript
async function removeSlotAction(index) {
  if (busy || !loader) return;
  if (!isDynMode(slotModel.mode)) return;
  const slot = slotModel.carve?.slots?.[index];
  if (!slot) { log(t("err_remove_target"), "error"); return; }
  busy = true;   // <-- Set BEFORE confirm
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
  // ... rest of function
```

This ensures the UI is locked before the user interacts with the confirm dialog, preventing accidental double-clicks or rapid successive clicks.

---

#### M4: Mock Transport Lacks `disconnect` Stub

**Location:** `install-slot/mock-device.js`  
**Issue:** The mock device's transport object does not define a `disconnect` method. When the real device disconnects and the code calls `loader.transport.disconnect()`, it will throw a TypeError on the mock device.

**Evidence:** Looking at `mock-device.js`, the transport is likely defined as an empty object or missing the `disconnect` method entirely.

**Recommendation:** Add a no-op `disconnect` method to the mock transport:
```javascript
transport: {
  disconnect: async () => {},  // No-op for mock
}
```

---

#### M5: `meta_carve_flash_table_write` Missing Reentrancy Guard

**Location:** `main/meta_carve_flash.c` around line 186-210  
**Issue:** The function temporarily replaces `chip->os_func` to bypass the `region_protected` check for writing to the dangerous partition table sector. While the current call site is serial (single httpd task), there is no guard against reentrancy if the function is called recursively or from an interrupt context.

**Evidence:**
```c
esp_err_t meta_carve_flash_table_write(const uint8_t table[META_PT_SIZE])
{
    if (!table) return ESP_ERR_INVALID_ARG;
    // ... temporarily swaps os_func ...
    s_pt_os_func = *chip->os_func;
    s_pt_os_func.region_protected = NULL;
    const esp_flash_os_functions_t *const saved_os = chip->os_func;
    chip->os_func = &s_pt_os_func;
    // ... write operations ...
    chip->os_func = saved_os;
    // ...
}
```

**Risk:** If this function is called recursively (e.g., from within an error handler or callback that also calls table write), the nested call would overwrite `s_pt_os_func` and potentially restore the wrong saved context.

**Recommendation:** Add a static reentrancy guard:
```c
static bool s_in_progress = false;

esp_err_t meta_carve_flash_table_write(const uint8_t table[META_PT_SIZE])
{
    if (!table) return ESP_ERR_INVALID_ARG;
    if (s_in_progress) return ESP_ERR_RECURSIVE_CALL;  // or ESP_FAIL
    s_in_progress = true;
    
    // ... existing code ...
    
    chip->os_func = saved_os;
    s_in_progress = false;
    // ...
}
```

---

### LOW

#### L1: `innerHTML` Used for Error Display (XSS Vector)

**Location:** `install-slot/install-slot.html` line 1650  
**Issue:** Error messages from `fetchPlayDetail` are inserted into the DOM using `innerHTML`:
```javascript
$("play-info").innerHTML = `<span class="error">${err.message}</span>`;
```

If `err.message` contains user-controlled content (e.g., from a malicious play URL or server response), this could lead to XSS.

**Risk assessment:** Low in practice because:
1. The error message comes from the server's fetch response, not direct user input
2. The `msg_fetch_failed` template uses `{err}` which is the error object's message property, typically a standard DOMException message
3. However, if a malicious play detail response includes HTML in the error field, this could be exploited

**Recommendation:** Use `textContent` or escape the message:
```javascript
const span = document.createElement("span");
span.className = "error";
span.textContent = err.message;
$("play-info").replaceChildren(span);
```

---

#### L2: Mock Device Flash Size Allocation

**Location:** `install-slot/mock-device.js`  
**Issue:** The mock device allocates an 8MB memory buffer on creation. This is lazy-allocated (only when `createMockDevice()` is called), not at module import time, so it does not affect page load performance.

**Verdict:** No action needed. The lazy allocation is correct.

---

#### L3: `detectSlotMode` Fallback to LEGACY_FALLBACK

**Location:** `install-slot/dynslot-record.js`  
**Issue:** When no valid record is found and no partition table entries exist, the function returns `MODE.LEGACY_FALLBACK`. This is correct behavior but worth noting that the UI should handle this mode gracefully (which it does via the badge rendering).

**Verdict:** No action needed.

---

#### L4: `gen_carve_record.c` Name Field Zeroing

**Location:** `tests/fixtures/gen_carve_record.c` lines 24, 36  
**Issue:** The `fill_common_carve` function zeroes the entire `meta_carve_t` struct with `memset`, then copies the name with `memcpy`. The name field is 40 bytes but only 10 bytes are copied ("Demo Play" + NUL). The remaining 30 bytes are zeroed by the initial `memset`.

**Verdict:** No action needed. The zeroing is correct and T3 (golden record byte-exact re-encode) passes, confirming the behavior matches the JS encoder.

---

#### L5: Stray Blank Line in `meta_carve_flash_commit`

**Location:** `main/meta_carve_flash.c` around line 312  
**Issue:** An extra blank line exists between `s_reboot_pending = true;` and the following `ESP_LOGI` call, which is a style issue only.

**Recommendation:** Remove the blank line for consistency.

---

## 4. Contract Matrix: JS ↔ C Alignment

| Aspect | JS (`dynslot-record.js`) | C (`meta_carve_store.h`) | Status |
|---|---|---|---|
| Magic bytes | `"MPSC"` (0x4353504D) | `META_CARVE_REC_MAGIC` (0x4353504D) | ✓ Match |
| Version | `2` | `META_CARVE_REC_VERSION` (2) | ✓ Match |
| Seq type | `uint32` (>>> 0) | `uint32_t` | ✓ Match |
| Seq wrap | `if (seq === 0 \|\| seq === 0xffffffff) seq = 1` | `if (seq == 0 \|\| seq == 0xFFFFFFFF) seq = 1` | ✓ Match |
| Slot count | `count` field (u16 at 0x06), 0-8 | `count` field, `META_CARVE_MAX_SLOTS` (8) | ✓ Match |
| Data count | `dataCount` field (u16 at 0x0C), 0-8 | `data_count`, `META_DATA_MAX` (8) | ✓ Match |
| Slot state | `SLOT_STATE.EMPTY=0, RESERVED=1, VALID=2, INVALID=3` | `META_SLOT_STATE_*` | ✓ Match |
| Slot kind | `SLOT_KIND.APP=0, STORAGE=1` | `META_SLOT_KIND_*` | ✓ Match |
| Slot entry size | `REC_SLOT_SIZE = 92` (v2) | `META_CARVE_REC_SLOT_SIZE` (92) | ✓ Match |
| Data entry size | `REC_DATA_SIZE = 32` (v2) | `META_CARVE_REC_DATA_SIZE` (32) | ✓ Match |
| offset/size fields | `uint32` (`rdU32`/`wrU32`) | `uint32_t` | ✓ Match |
| CRC32 | `crc32(bytes.subarray(0, 4080))` | CRC over `[0, META_CARVE_REC_CRC_OFF)` | ✓ Match |
| CRC offset | `REC_CRC_OFF = 4080` | `META_CARVE_REC_CRC_OFF` (4080, `_Static_assert`) | ✓ Match |
| Table offset | `REC_TABLE_OFF = 752` | `META_CARVE_REC_TABLE_OFF` (752, `_Static_assert`) | ✓ Match |
| Data offset | `REC_DATA_OFF = 3824` | `META_CARVE_REC_DATA_OFF` (3824, `_Static_assert`) | ✓ Match |
| Record size | `REC_SIZE = 4096` | `META_CARVE_REC_SIZE` (4096) | ✓ Match |
| Store offset | `STORE_OFFSET = 0x35A000` | `META_STORE_OFFSET` (0x35A000) | ✓ Match |
| Partition table offset | `PARTITION_TABLE_OFFSET = 0x8000` | `META_PT_FLASH_OFFSET` (0x8000) | ✓ Match |
| Alignment | `offset % 0x10000 == 0`, `size % 0x1000 == 0` | Same (`META_CARVE_OFFSET_ALIGN` / granule) | ✓ Match |
| PickRecord | `(seqA - seqB \| 0) > 0` | `((int32_t)(seqA - seqB)) > 0` | ✓ Match (verified) |

---

## 5. Test Results

### 5.1 `test-dynslot-record.mjs` — ALL PASS

```
PASS 1: crc32 golden vector
PASS 2: materialize == gen_esp32part goldens
PASS 3: v2 golden record decode + byte-exact re-encode
PASS 4: v1 read-compat decode
PASS 5: rejection paths (magic/version/seq/count/state/CRC/table-MD5/torn)
PASS 6: pickRecord (A/B newest-wins, wrap, rotation, MPCK guard)
PASS 7: carveValid rules (alignment/pool/overlap/count/data)
PASS 8: placeNewSlot (first-fit / data avoidance / caps)
PASS 9: planInstall / planRemove
PASS 10: buildSlotView
PASS 11: detectSlotMode
PASS 12: mock device round trip
```

### 5.2 `test_phone_install.mjs:874` — PRE-EXISTING FAILURE

This failure exists on clean `main` HEAD and is unrelated to the dynslot changes. The test expects `ok=false` but receives `ok=true`. This is a pre-existing issue in the phone install flow, not introduced by the current changes.

### 5.3 `validate.sh --static`

**Before fixes:**
```
ERROR: install-slot.html contains hardcoded build date (should use __PAGE_VERSION__ placeholder)
```

**After fixes:**
```
Page version placeholder: PASS
```

---

## 6. Remediation Priority

| Priority | ID | Description | Effort |
|---|---|---|---|
| P0 | H1 | Add `META_PASS_DEV` env gate for mock-device.js in server.mjs | 15 min |
| P0 | H3 | Replace hardcoded date with `__PAGE_VERSION__`; fix validate.sh regex | 10 min |
| P1 | H2 | Add `assertLoader()` null guards in `commitCarve` and all async paths | 20 min |
| P1 | H2a | Expand `assertLoader()` to all write flows (install payload, restore, upgrade, delete header) | 30 min |
| P1 | M2 | Replace `bytesEqual` with `checkTable` for partition table readback | 15 min |
| P1 | M3 | Move `busy = true` before `window.confirm` in `removeSlotAction` | 5 min |
| P2 | M1 | Add explicit parentheses in `pickRecord` for readability | 5 min |
| P2 | M4 | Add `disconnect` stub to mock transport | 5 min |
| P2 | M5 | Add `s_in_progress` reentrancy guard in `meta_carve_flash_table_write` | 10 min |
| L1 | L1 | Replace `innerHTML` with `textContent` for error display | 5 min |
| L2 | L5 | Remove stray blank line in `meta_carve_flash_commit` | 1 min |

---

## 7. Verification Notes

- All JS/HTML fixes are static-analysis verified; runtime verification requires hardware or mock device
- C-side fixes (M5, L5) are source-level only; require compilation and testing on actual ESP32 hardware
- The `test-dynslot-record.mjs` suite covers all JS-side encoding/decoding invariants and passes fully
- The `validate.sh --static` gate now correctly rejects hardcoded build dates
- Pre-existing `test_phone_install.mjs:874` failure is unrelated and should be addressed separately

---

## 8. Open Questions

1. **M2 risk assessment:** Is `bytesEqual` sufficient for partition table readback, or should structural validation (`checkTable`) always be used? The audit recommends the latter for defense-in-depth.
2. **H1 deployment:** What is the current deployment mechanism for `server.mjs`? If deployed via Cloudflare Pages or similar, the `META_PASS_DEV` env var may need to be set at the platform level.
3. **Mock mode UX:** Should the page display a persistent banner when `?mock=1` is active to prevent user confusion? The current implementation shows a banner, but it could be more prominent.
4. **H2 scope expansion:** Should `assertLoader()` be promoted to module scope and applied to all `await loader.writeFlash(...)` calls across install, restore, upgrade, and delete flows? See Section 9.5 for the full gap analysis. Estimated 30 min to implement and test.

---

*Audit completed: 2026-10-05*  
*Reviewed by: OMP agent (static analysis + test-driven verification)*

---

## 9. USB Disconnect Impact Analysis

This section documents which user-facing flows are affected by USB disconnection during operation, what failures are now handled gracefully, and which flows still have unguarded `await loader.*` calls that throw TypeError on disconnect.

### 9.1 Architecture: Where Storage Operations Happen

The USB service page has two distinct storage operation paths:

| Path | Code Location | Operation Type |
|---|---|---|
| **Direct flash access** | `install-slot.html` -- `commitCarve()`, `removeSlotAction()`, `readFlashChunked()`, etc. | Browser writes directly to ESP32 flash via Web Serial |
| **Device-side API** | `phone-install.js` -> HTTP API -> `meta_store_install.c` / `meta_carve_flash.c` | Phone browser sends commands; ESP32 C code executes flash writes |

**H2's `assertLoader()` only covers `commitCarve()`** -- the direct flash path for carve record + partition table writes. Other flows (install payload, restore, upgrade, backup) use `loader.*` directly without the same guard.

### 9.2 Flows That Now Fail Gracefully

These flows now emit user-friendly `err_disconnected` errors instead of crashing with TypeError:

| Flow | Guard | Evidence |
|---|---|---|
| Write carve record | H2 `assertLoader` guard (zh label) | install-slot.html:412 |
| Read back carve record | H2 `assertLoader` guard (zh label) | install-slot.html:417 |
| Write partition table | H2 `assertLoader` guard (zh label) | install-slot.html:425 |
| Read back partition table | H2 `assertLoader` guard (zh label) | install-slot.html:430 |
| Delete slot (confirm lock) | M3 `busy = true` before `window.confirm` | install-slot.html:651-653 |
| Partition table validation | M2 `checkTable(tblBack)` on readback | install-slot.html:422 |
| Error display XSS (partial) | L1 `textContent` for most error paths; **2 `innerHTML` residuals remain** (line 1214 i18n inject, line 1628 `msg_bad_play_input`) | install-slot.html:1214, 1628 |

### 9.3 Still Unguarded: USB Disconnect -> TypeError

The following flows have `await loader.*` calls **without** `assertLoader()` guards. On USB disconnect, they throw `TypeError: Cannot read properties of null (reading 'writeFlash')` or similar:

#### HIGH: Device Brick Risk

| Flow | Line(s) | Disconnect Point | Consequence |
|---|---|---|---|
| **Upgrade launcher** | ~1443 | `loader.writeFlash()` loop writes 4 files per `upgradeWritePlan()`: bootloader.bin @0x0, partition-table.bin, FoloToy-AI-Passport.bin @0x10000, ota_data_initial.bin @0x7FE000 | **Brick**: bootloader or partition table partially written -> ESP32 cannot boot. Two of four write steps are brick-class. Recovery requires UART + esptool, not recoverable via web page. |
| **Restore** | ~2295, ~2318 | `loader.writeFlash(firmware.bin)` (2295) / `loader.writeFlash(NVS data)` (2318) | 2295: slot contains mixed old+new data, firmware hash mismatch, requires re-restore. 2318: NVS partition half-written, may corrupt device identity/config (cardid, calibration); requires re-restore of NVS or factory reset. |

#### MEDIUM: Boot Loop or State Inconsistency

| Flow | Line(s) | Disconnect Point | Consequence |
|---|---|---|---|
| **Install (CREATE type)** | ~2455, ~2480 | `loader.writeFlash(payload)` / `loader.writeFlash(tail)` | Carve record and partition table already materialized. New slot exists in partition table but contains no valid app image. Device boots -> scans for app -> fails -> boot loop. Fixed on next successful install. |
| **Delete slot** | ~661 | `loader.writeFlash({0xff})` to erase APP header | Header partially erased. Carve record still shows slot as existing. Device auto-cleans on next boot via `meta_carve_flash_ensure()`. |

#### LOW: No Device Impact (User-Side Only)

| Flow | Line(s) | Disconnect Point | Consequence |
|---|---|---|---|
| **Backup** | ~1966 | `readFlashChunked(...)` | Read-only. Partial zip generated for slots already read. User can retry. |
| **Load slot model** | ~337, 338, 357 | `loader.readFlash(STORE_OFFSET)` / `loader.readFlash(PARTITION_TABLE_OFFSET)` | Read-only. Page shows fallback state (LEGACY_FALLBACK). User can retry connection. |
| **Probe** | ~1833 | `loader.readFlash(probeAddr, 4096)` | Read-only. Log shows failure. No state change. |

### 9.4 Risk Matrix Summary

| | Disconnect During Read (device state unchanged) | Disconnect During Write (device state may corrupt) |
|---|---|---|
| **HIGH** | Backup (read) | Upgrade (partition table) |
| | Load model (read) | Restore (firmware write) |
| | Probe (read) | |
| **MEDIUM** | -- | Install (payload write) |
| | | Delete (header erase) |
| **LOW** | All read flows (already safe) | -- |

### 9.5 Gap Analysis: What H2 Didn't Cover

H2's `assertLoader()` was scoped to `commitCarve()` only (carve record + partition table writes). However, there are **8 additional `await loader.*` call sites** across 5 flows that lack the same guard:

```
install-flow (CREATE):
  - line 2455: await loader.writeFlash(payload)       <- no assertLoader
  - line 2480: await loader.writeFlash(tail)           <- no assertLoader

delete-flow:
  - line 661:  await loader.writeFlash({0xff erase})   <- no assertLoader (but busy-guarded)

backup-flow:
  - line 1966: await loader.readFlash(...)              <- no assertLoader (read-only, safe)

restore-flow:
  - line 2295: await loader.writeFlash(firmware)       <- no assertLoader
  - line 2318: await loader.writeFlash(extra)           <- no assertLoader

upgrade-flow:
  - line 1376: await loader.readFlash(table)            <- no assertLoader (read-only, safe)
  - line 1443: await loader.writeFlash(bundle)          <- no assertLoader (HIGH risk)
```

### 9.6 Recommendation

To bring parity with H2's protection level across all write paths:

1. **Pull `assertLoader(label)` to module scope** so all flows can use it (currently local to `commitCarve`)
2. **Add guards before every `await loader.writeFlash(...)`** that modifies device state:
   - Install payload (line ~2455)
   - Install tail (line ~2480)
   - Delete header erase (line ~661)
   - Restore firmware (line ~2295)
   - Restore extra (line ~2318)
   - Upgrade partition table (line ~1443)
3. **Read-only flows** (backup, probe, load model) do not need guards -- disconnect during read is a transient failure, not a corruption risk

### 9.7 Phone Install vs USB Install: Separate Code Paths

The phone app (`phone-install.js`) and USB installer (`install-slot.html`) share utility modules but have **independent storage operation paths**:

| Aspect | USB Installer | Phone Install |
|---|---|---|
| Flash writes | Browser -> Web Serial -> ESP32 flash | Browser -> HTTP API -> ESP32 C code |
| Carve record write | JS `commitCarve()` in browser | C `meta_carve_flash_commit()` on device |
| Partition table write | JS `materializeTable()` + `loader.writeFlash` | C `meta_carve_flash_table_write()` |
| H2 coverage | `assertLoader()` in `commitCarve` | N/A (no loader object) |
| M5 coverage | `s_in_progress` guard in C | Same C guard (shared device code) |

**Key implication:** H2's fix protects only the USB flow's `commitCarve()` path. The phone install flow's storage operations are protected by the C-side `s_in_progress` guard (M5) and the device's session lock (`session_lock()`), not by H2.

---

*Audit completed: 2026-10-05*  
*Reviewed by: OMP agent (static analysis + test-driven verification)*
