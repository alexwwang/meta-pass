<p align="right">
  <a href="dynslot-usb-installer-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>
# USB Installer Dual-Mode: Dynslot vs Fixed-Slot

Date: 2026-10-03
Branch: `feat/dynslot`
Parent: `dynslot-design.md` §4.5 L4

## 1. Problem

The USB installer (`launcher-upgrade.js` + `install-slot.html` step 7)
currently assumes a single upgrade path: write bootloader + partition-table
+ factory app + otadata to fixed offsets, then compare the device's live
partition table with the bundle's table byte-for-byte. This works for
fixed-slot devices but breaks for dynslot devices because:

1. **Partition tables differ structurally**: fixed-slot devices declare
   `ota_0/1/2` as app partitions; dynslot devices declare `pool_0/1` as
   data placeholders with the launcher materializing the carved table at
   runtime. A byte-for-byte comparison always fails cross-version.
2. **Upgrade scope differs**: dynslot devices must preserve the entire
   pool (including the carved table snapshot in store), while fixed-slot
   devices preserve only the three ota_* regions.
3. **Erase plans differ**: fixed-slot migration erases `0x180000→0x7FE000`;
   dynslot migration erases only legacy residue (if any) while preserving
   the pool contents.
4. **No version signal**: the phone has no way to know whether the connected
   device runs fixed-slot or dynslot firmware before committing to an
   upgrade path.

Goal: make the USB installer support both modes, selected by the device's
firmware version (or equivalent capability probe), so users can upgrade
from any version without switching tools.

## 2. Design decisions (resolved)

| # | Decision | Rationale |
|---|---|---|
| 1 | **Probe via `/api/install/slots`** (not HTTP header / new endpoint) | Existing call; add one version field to JSON response; backward-compatible (parseSlots ignores unknown fields). |
| 2 | **Gate by `protocol_version` integer** (not semver string) | Integer is unambiguous, easy to parse, no date/version-format corner cases. Fixed-slot = `1`, dynslot = `2`. |
| 3 | **Default to fixed-slot when probe fails** | Safe failure mode: if the device is too old to answer `/api/install/slots`, fall back to the legacy upgrade path (which also works on blank devices via the migration path). |
| 4 | **Two distinct write plans, not one parameterized plan** | Simpler to test, clearer error messages, easier to maintain independently. Cross-contamination between modes is the source of bugs. |
| 5 | **Migration path is a third, separate code path** | Legacy FoloToy → dynslot is a one-time migration, not the common case. Keep it distinct from the routine upgrade flow. |

## 3. Version probe

### 3.1 Protocol version field

Add `protocol_version` to the `/api/install/slots` JSON response:

```json
{
  "protocol_version": 2,
  "count": 3,
  "free": 1234567,
  "archived": 0,
  "slots": [...]
}
```

**Schema:**

- `protocol_version`: `1` = fixed-slot (pre-dynslot), `2` = dynslot.
  Omitted for backwards compatibility with older devices (treated as `1`).
- All existing fields (`count`, `free`, `archived`, `slots`) unchanged.

**C-side change** (`main/meta_store_install.c`, `h_install_slots`):

```c
off += snprintf(resp + off, sizeof(resp) - off,
    "{\"protocol_version\":%d,\"count\":%d,\"free\":%" PRIu32 ",...",
    META_PROTOCOL_VERSION, carve->count, free_bytes, archived_count);
```

Where `META_PROTOCOL_VERSION` is a compile-time constant in `meta_carve.h`:

```c
#define META_PROTOCOL_VERSION 2  // incremented when carving protocol changes
```

**JS-side** (`install-slot/phone-install.js`, `parseSlots`):

```js
export function parseSlots(text) {
  let d = null;
  try { d = JSON.parse(text); } catch { return null; }
  if (!d || !Number.isInteger(d.count) || d.count < 0 || d.count > 8 ||
      !Number.isFinite(d.free) || !Array.isArray(d.slots) ||
      d.slots.length !== d.count) return null;
  // NEW: optional protocol_version (defaults to 1 for legacy devices)
  const protocolVersion = Number.isInteger(d.protocol_version)
    ? d.protocol_version : 1;
  // ... existing slot validation ...
  return { count: d.count, free: d.free, slots, protocolVersion };
}
```

### 3.2 Fallback probe: partition table inspection

If the device does not respond to `/api/install/slots` (e.g., still in
pairing mode, or very old firmware), fall back to reading the partition
table directly:

```js
// launcher-upgrade.js
// NOTE: pool_0/pool_1 cannot be used as the criterion — the live table on a
// running device is the CARVED table, where pool placeholders have been
// replaced by ota_N slot entries + data records; pool_* exists only in the
// safe table (factory / not-yet-carved state). The stable criterion is the
// store@0x35A000 entry (carve record A/B area, a FIXED member present on
// both table flavours); legacy fixed 3-slot tables and stock FoloToy tables
// have no store partition.
export function isDynslotLayout(devicePartitions) {
  return devicePartitions.some(p => p.label === 'store' && p.offset === 0x35A000);
}
```

Detection order (maps to protocol version): store entry present → dynslot (2);
else ota_0/ota_1/ota_2 present → fixed-slot (1);
else factory only (no ota_*) → legacy FoloToy (0); anything else → unknown (-1).

This fallback is used only when the slots API is unreachable.

## 4. Dual-mode write plans

### 4.1 Fixed-slot mode (`protocol_version === 1`)

**Unchanged from current behavior.** The existing `upgradeWritePlan()`
returns four steps:

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

Table comparison: bundle table must match device table byte-for-byte
(both are the fixed 3-slot table). Migration path (`migrationErasePlan()`)
handles the factory-only → fixed-slot transition.

### 4.2 Dynslot mode (`protocol_version === 2`)

**New write plan.** The partition table in the bundle is the **safe table**
(with pool_0/pool_1 as data placeholders, no carved app entries). The
launcher on-device will materialize the carved table from the store record
after the upgrade.

```js
// In launcher-upgrade.js, conditional export:
export function upgradeWritePlan(protocolVersion) {
  if (protocolVersion >= 2) {
    return [
      { name: "bootloader.bin",      offset: 0x0,        why: "second-stage bootloader" },
      { name: "partition-table.bin", offset: 0x8000,     why: "safe table (carved at boot)" },
      { name: "FoloToy-AI-Passport.bin", offset: 0x10000, why: "factory app" },
      { name: "ota_data_initial.bin", offset: 0x7FE000,  why: "reset OTA selection" },
    ];
  }
  // Fixed-slot path (unchanged)
  return [
    { name: "bootloader.bin",       offset: 0x0,        why: "second-stage bootloader" },
    { name: "partition-table.bin",  offset: 0x8000,     why: "layout contract" },
    { name: "FoloToy-AI-Passport.bin", offset: 0x10000, why: "factory app" },
    { name: "ota_data_initial.bin", offset: 0x7FE000,  why: "reset OTA selection" },
  ];
}
```

**Table comparison rule (dynslot):** Byte-for-byte equality is impossible
(the carved table on-device is stateful). Compare at the **entry level**:
the FIXED entries (nvs, phy_init, factory, cardid, store, otadata) must be
identical (type/subtype/offset/size) on both sides; carved ota_N slots and
data records are dynamic content and are not compared. The bundle must
carry the pool placeholders (pool_0/pool_1) — this gates out flashing a
fixed-slot bundle onto a dynslot device.

```js
export function comparePartitionTables(deviceTable, bundleTable, protocolVersion) {
  if (protocolVersion >= 2) {
    // Dynslot: verify bundle is a valid safe table (no app partitions)
    // and protected regions match. The carved table on device will differ.
    const bundleParts = parsePartitionTable(bundleTable);
    const labels = new Set(bundleParts.map(p => p.label));
    if (!labels.has('pool_0') || !labels.has('pool_1')) {
      return { ok: false, reason: "bundle table is not a dynslot safe table" };
    }
    // Compare protected regions only
    return compareProtectedRegions(deviceTable, bundleTable);
  }
  // Fixed-slot: existing byte-for-byte comparison
  return _comparePartitionTablesFixed(deviceTable, bundleTable);
}
```

### 4.3 Erase plan differences

**Fixed-slot mode** (`migrationErasePlan()` — unchanged):
Erases `0x180000→0x7FE000` (legacy residue from factory-only devices).

**Dynslot mode** (new):
No erase needed for routine upgrades (pool content preserved across
launcher updates). Erase only on first-time migration from fixed-slot:

```js
export function upgradeErasePlan(protocolVersion, devicePartitions) {
  if (protocolVersion >= 2) {
    // Dynslot: check if device still has fixed-slot layout (migration case)
    const labels = new Set(devicePartitions.map(p => p.label));
    if (labels.has('ota_0') && labels.has('ota_1') && labels.has('ota_2')) {
      // Device has fixed-slot table but running dynslot firmware — migrate
      return [{ offset: 0x180000, size: 0x680000, why: "migrate fixed→dynslot pool" }];
    }
    return []; // No erase needed
  }
  return migrationErasePlan(); // Fixed-slot path (unchanged)
}
```

## 5. UI flow

### 5.1 Mode detection sequence

```
User clicks "Upgrade launcher"
  │
  ├─ 1. Read device partition table (0x8000, 0x1000 bytes)
  │     → parsePartitionTable() → devicePartitions
  │
  ├─ 2. Try /api/install/slots (requires WiFi pairing session)
  │     ├─ Success → parseSlots() → protocolVersion from JSON
  │     └─ Failure → detectProtocolFromTable(devicePartitions)
  │
  ├─ 3. Dispatch to mode-specific flow
  │     ├─ protocolVersion == 2 → dynslot upgrade path
  │     ├─ protocolVersion == 1 → fixed-slot upgrade path
  │     └─ protocolVersion == 0 → legacy migration path
  │
  └─ 4. Show mode indicator in UI ("Upgrading launcher (dynslot mode)")
```

### 5.2 UI changes

**`install-slot.html`** — add mode indicator:

```html
<p id="upgrade-mode-indicator" class="info" style="display:none">
  Mode: <span id="upgrade-mode-text"></span>
</p>
```

**JavaScript** — set indicator after probe:

```js
const protocolVersion = slotsData?.protocolVersion ??
  detectProtocolFromTable(deviceTable);
const modeLabel = protocolVersion >= 2 ? "Dynslot" :
                  protocolVersion === 1 ? "Fixed-slot" : "Legacy";
$("upgrade-mode-text").textContent = modeLabel;
$("upgrade-mode-indicator").style.display = "block";
```

**i18n** — add strings:

```js
upgrade_mode_dynslot: "Dynslot mode (dynamic slots)",
upgrade_mode_fixed: "Fixed-slot mode (3 slots)",
upgrade_mode_legacy: "Legacy mode (migration)",
```

## 6. Bundle format

The release binary (`meta-pass_v*.bin`) already contains the safe table
(dynslot) or fixed table (fixed-slot) at `0x8000`. **No bundle format
change needed** — the same hybrid container serves both modes. The
difference is in how the phone interprets and validates the table.

**Verification gate** (`tools/verify_firmware.py`): already updated to
check pool_0/pool_1 as preserved regions (§4.6 of dynslot-design.md).

## 7. Migration path (legacy fixed-slot → dynslot)

One-time migration when a fixed-slot device installs a dynslot launcher:

1. Table probe: device partitions have `ota_*` and no `store`
   (`isFixedSlotLayout`); bundle partitions are a dynslot safe table
   (`isDynslotSafeTable`: pool_0/pool_1 + store).
2. Phone logs the migration notice: partition table stays untouched,
   installed games preserved.
3. Write plan switches to `migrationWritePlan()`: **bootloader + factory
   app + otadata only — `partition-table.bin` is deliberately NOT
   written**, and no erase runs.

**Why no table write / no erase** (corrected from the original draft of
this section): `meta_carve_flash_ensure()` only seeds the carve from the
legacy layout when the live table still *is* the legacy table
(`meta_pt_equal(live, meta_pt_legacy())` → `meta_carve_seed_legacy`,
"plays untouched"). Overwriting the table with the safe table makes the
first boot take the "fresh device" path — installed games become orphan
bytes the carve never references (pool reads empty, next install
overwrites them). Likewise `migrationErasePlan()` targets
`0x180000→0x7FE000`, which is exactly where the installed games live;
running it would destroy the very data the migration promises to keep.

4. Device boots → `ensure()` sees the intact legacy table, no carve
   record → `seed_legacy` rebuilds the carve at identical offsets/sizes
   → commit materializes the carved table. All games remain bootable.

**Data safety**: `meta_carve_seed_legacy()` preserves every existing app
image at its original offset. Worst case across a power cut: the record
or the table is re-derived on next boot by the same idempotent path
(design §4.7).

## 8. Error handling

| Scenario | Response |
|---|---|
| Device unreachable | "Connect device first" (existing) |
| Protocol probe fails | Fall back to partition table inspection |
| Unknown protocol | Reject with "Unsupported firmware version" |
| Bundle table invalid for detected mode | "Firmware bundle incompatible with this device" |
| Migration erase conflicts (data in pool) | "Backup existing games first" (same as current slot-has-data check) |

## 9. Test plan

### 9.1 Unit tests (`launcher-upgrade.js`)

```js
// tests/test_launcher_upgrade.mjs (new file)
import { upgradeWritePlan, comparePartitionTables, detectProtocolFromTable, upgradeErasePlan } from "../install-slot/launcher-upgrade.js";

// Test 1: write plan dispatch
assert.equal(upgradeWritePlan(1).length, 4);  // fixed
assert.equal(upgradeWritePlan(2).length, 4);  // dynslot (same steps, different table semantics)

// Test 2: protocol detection from table
const fixedTable = buildTable(["nvs", "factory", "ota_0", "ota_1", "ota_2", "otadata"]);
const dynslotTable = buildTable(["nvs", "factory", "pool_0", "pool_1", "store", "otadata"]);
const legacyTable = buildTable(["nvs", "factory", "recovery"]);

assert.equal(detectProtocolFromTable(fixedTable), 1);
assert.equal(detectProtocolFromTable(dynslotTable), 2);
assert.equal(detectProtocolFromTable(legacyTable), 0);

// Test 3: table comparison — fixed mode (byte-equal)
// Test 4: table comparison — dynslot mode (protected regions only)
// Test 5: erase plan — fixed slot upgrade (no erase)
// Test 6: erase plan — dynslot routine upgrade (no erase)
// Test 7: erase plan — migration fixed→dynslot (erase pool region)
```

### 9.2 Integration tests

- USB upgrade: fixed-slot device → fixed-slot bundle (existing path, green)
- USB upgrade: dynslot device → dynslot bundle (new path)
- USB upgrade: fixed-slot device → dynslot bundle (migration path)
- USB upgrade: dynslot device → fixed-slot bundle (reject: incompatible)

### 9.3 Backward compatibility

- Old devices (no `/api/install/slots`) continue to work via partition
  table fallback.
- Old bundles (fixed-slot table) rejected by dynslot devices with clear
  error message.
- New bundles (safe table) rejected by fixed-slot devices (table mismatch
  caught by existing comparison).

## 10. Implementation order

1. **Add `protocol_version` to slots API** (C + JS parseSlots update).
2. **Add `detectProtocolFromTable()`** to `launcher-upgrade.js`.
3. **Parameterize `upgradeWritePlan()`** with protocol version (backward-compatible: default to fixed-slot).
4. **Parameterize `comparePartitionTables()`** with protocol version.
5. **Add `upgradeErasePlan()`** for migration detection.
6. **Update HTML UI** with mode indicator and i18n strings.
7. **Add tests** (`test_launcher_upgrade.mjs`).
8. **Smoke test** on real device (fixed-slot → dynslot upgrade).

## 11. Open questions

- **Q1**: Should we add a `/api/info` endpoint that returns `protocol_version`
  without requiring an active install session? (Currently slots API
  requires token; info endpoint could be public.)
  → **Answer**: Not needed. Slots API is called during the pairing session
  which is already active when upgrade is initiated.

- **Q2**: What happens if a user flashes a mixed bundle (fixed-slot table
  onto dynslot device, or vice versa)?
  → **Answer**: Table comparison rejects it. The safe-table validation in
  dynslot mode checks for pool_0/pool_1 presence.

- **Q3**: Should the protocol version be bumped when M5 (data lifecycle)
  changes the store format?
  → **Answer**: No — M5 is backward-compatible (new fields, old parsers
  ignore them). Only structural table changes warrant a version bump.
