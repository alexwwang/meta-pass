<p align="right">
  <a href="dynslot-m5-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# M5 — Detached Data-Carve Lifecycle (Detailed Design)

Date: 2026-10-03
Branch: `feat/dynslot`
Parent: `dynslot-data-unification-research.md` §4 M5,
        `dynslot-design.md` §4.5 reclaim ladder tier 3–5.

## 1. Scope and current state

M5 makes the play's flash-attached user data (recordings, saves, archives)
survive **upgrade** (re-install of same slug) and **uninstall** (default
ARCHIVED until user erases or pool pressure reclaims).

### 1.1 Already built (schema + low-level helpers)

- `meta_carve.h` data struct (v2, 32 bytes):
  ```c
  typedef struct {
      uint32_t play_id;   // >0; key for lifecycle (0 = v1 orphan, ignored)
      uint32_t offset;    // 64 KB-aligned
      uint32_t size;      // 4 KB granularity, ≥ META_CARVE_MIN_DATA (4 KB)
      uint8_t  state;     // meta_data_state_t
      uint8_t  subtype;   // child-declared (0x81 fat / 0x82 spiffs / …);
                          // 0 = DATA_OTA rejected by meta_carve_valid
      uint8_t  type;      // must be 1 (data)
      char     label[META_DATA_LABEL_MAX + 1];  // ≤ 16 B, NUL-terminated
  } meta_carve_data_t;
  ```
- State enum: `PRISTINE=0`, `DIRTY=1`, `ARCHIVED=2`.
- `meta_carve_place_data(cur, size, &offset)` — first-fit placement; rejects
  if size invalid or no room.
- `meta_carve_find_data(cur, play_id, label)` — keyed lookup; -1 if none.
- `meta_carve_data_append/remove` — raw CRUD (calls go through
  `meta_carve_place_data` + invariant guard).
- `meta_carve_reclaimable(c)` = sum of bytes of all PRISTINE + ARCHIVED
  records (DIRTY is "in use" and excluded).
- Store layout: `data[8]` rows @ 0x37A0–0x3FB0 of each 4 KB sector, CRC
  over `[0, 4032)` — already handles these fields; decoding rejects
  `type≠1`, `subtype=0`, `play_id=0`, or `state>ARCHIVED` (§25 of
  `meta_carve_store.h`).

### 1.2 Implementation status (2026-10-03)

The schema, allocator, and lifecycle API are implemented and covered by host
tests in `tests/test_meta_carve_flash.c`. The remaining gap is the install-flow
wiring that would create data records and drive the reclaim ladder.

| Designed item | State | Evidence |
|---|---|---|
| `meta_carve_flash_set_dirty(play_id)` | Done | `meta_carve_flash.h`; called at finalize `meta_store_install.c:666` |
| `meta_carve_flash_archive_slot_and_data(int slot)` | Done | declared/implemented; called from `h_install_remove` `meta_store_install.c:1188` |
| `meta_carve_flash_erase_data(play_id, label)` | Done, **no caller** | implemented; no HTTP route yet |
| `meta_carve_flash_arc(target)` | Partial wiring | implemented; called only from `/api/backup/import` `meta_store_install.c:1386`; install no-fit path not wired |
| `meta_carve_flash_data_copy` | Done | implemented; called at finalize `meta_store_install.c:652` |
| Manifest `data[]` parse | Done | `meta_install_model.c:131` (`play_id`/`size`/`label`; constant is `META_DATA_MAX`, not `META_MANIFEST_DATA_MAX`) |
| Uninstall archival | Done | `h_install_remove` archives, then removes the slot |
| DIRTY marking trigger | Done (finalize path) | §5 decision; no launch-time hook, by design |
| Data-record creation on install | **Missing** | no production caller of `meta_carve_place_data` |
| ARC on install no-fit | **Missing** | `meta_carve_flash_arc` not called on the install path; `meta_carve_largest_gap`/`meta_carve_reclaimable` have no callers |

Lifecycle state flips persist across reboot (asserted by a simulated restart in
`tests/test_meta_carve_flash.c`).

### 1.3 What this document covers

- The state machine and its guards.
- Three touch points with exact call sites, pre-conditions, and
  failure modes.
- New public API (all in `meta_carve_flash.h`).
- Upgrade copy semantics (when to copy, when to reject).
- Pool-pressure ARC (tier 3 of the reclaim ladder).
- Edge cases: v1 orphan data, `play_id=0`, signature-bound PRISTINE,
  power-loss mid-copy, multi-play label sharing (M4 generalized rule).

## 2. State machine

```text
          install            launch(ok long-press)        uninstall
PRISTINE ─────────────────► DIRTY ──────────────────────► ARCHIVED
   │                              ▲                            │
   │  upgrade copies              │  erase/reset data          │  ARC by pool pressure
   │  PRISTINE→PRISTINE           │  or DIRTY→DIRTY            │  or user explicit erase
   │                              │                            │
   └─── upgrade: no data records ─┘
       → fresh PRISTINE
```

Transitions and their invariants:

| Transition | Guard | Action | Failure mode |
|---|---|---|---|
| **install → PRISTINE** | `meta_carve_place_data` places; content restored by app write or phone re-send | record `state=PRISTINE` | pool full → `no-fit`; rejected by UI |
| **finalize success → DIRTY** | install finalized (§5 decision) | flip `state=DIRTY` in store; no erase | — (best-effort; failure is silent) |
| **upgrade → copy PRISTINE or fresh PRISTINE** | see §4 | copy bytes in pool, or drop + re-PRISTINE on next install | size shrink (new < old) → reject; size grow → in-pool copy + re-commit table |
| **uninstall → ARCHIVED** | `meta_carve_flash_archive_slot_and_data` | flip `state=ARCHIVED`; keep bytes in flash | — |
| **pool-pressure → erase ARCHIVED** | `meta_carve_flash_arc` (see §7) | commit record, then erase bytes; remove record | none — best-effort loop |
| **explicit erase** | UI choice at remove-confirm | erase bytes; remove record | none |

`DIRTY` never transitions back — persistent across reboot. Only uninstall or
ARC moves it to `ARCHIVED`.

## 3. play_id derivation

- **Phone-side** (analyze): `play.revisionId` from the catalog — a stable
  integer across versions of the same play. Used in propose offer:
  `manifest.play_id`.
- **Device-side** (prepare/offline re-install): lookup by `manifest.slug`
  → decode from catalog cache (phone-side pre-populates into the manifest
  JSON payload, so device-side is stateless on this field). Fallback:
  `play_id=0` if the field is absent (v1 compatibility path; §6.3).
- **Invariant**: `play_id=0` records are treated as legacy/v1 orphans — no
  lifecycle action touches them. They survive in the store but never get
  DIRTY'd, ARCHIVED, or ARC'd. (Migration seed from v1 carve sets
  `play_id=0` for those records.)

## 4. Touch point 1: install / upgrade

### 4.1 Install path (`meta_store_install.c:472 slot_materialize_locked`)

Current shape (after dynslot migration):

```c
// line 487-503: new slot branch
if (!s_session.manifest_valid || !s_session.manifest.has_carve) return ...;
meta_carve_t next;
if (meta_carve_place(cur, s_session.manifest.carve_size,
                     META_CARVE_KIND_APP, &next) != slot ||
    !meta_carve_valid(&next)) { ... }
if (meta_carve_flash_commit(&next, true) != ESP_OK) { ... }
```

Add before `meta_carve_flash_commit`:

```c
// Upgrade data migration (M5, install/upgrade touch point):
// For each declared data record in the manifest, look up the existing
// carve by (play_id, label). If found and state ∈ {PRISTINE, DIRTY}:
//   - new_size >= old_size: copy old bytes to new offset in pool, update
//     record in 'next', erase old offset (optional, deferred to ARC),
//     keep state.
//   - new_size < old_size: REJECT (user must manually archive first via
//     explicit erase, or remove + re-install without data).
// If not found: fresh PRISTINE — no migration needed.
// V1 records (play_id=0): skip migration; let them land PRISTINE on next
// install write.
for (int i = 0; i < s_session.manifest.data_count; i++) {
    const meta_manifest_data_t *req = &s_session.manifest.data[i];
    int j = meta_carve_find_data(cur, req->play_id, req->label);
    if (j < 0) continue;  // fresh install — no action
    const meta_carve_data_t *old = &cur->data[j];
    if (req->size < old->size) {
        // size shrink — reject and report
        ESP_LOGW(TAG, "data shrink rejected: play_id=%u label=%s %u->%u",
                 (unsigned)req->play_id, req->label,
                 (unsigned)old->size, (unsigned)req->size);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (req->size == old->size && old->offset == req->offset) {
        // no change — skip copy
        memcpy(&next.data[next.data_count++], old, sizeof(*old));
        continue;
    }
    // copy bytes in pool (best-effort):
    esp_err_t cp = meta_carve_flash_data_copy(old->offset, old->size,
                                              req->offset);
    if (cp != ESP_OK) {
        // copy failed (e.g. bad address, power-loss recovery in flight) —
        // abort upgrade, leave old record intact, fall back to fresh PRISTINE
        // on next install attempt.
        ESP_LOGE(TAG, "data copy failed: %s", esp_err_to_name(cp));
        return ESP_ERR_NOT_SUPPORTED;
    }
    meta_carve_data_t d = *old;
    d.offset = req->offset;
    d.state = META_DATA_PRISTINE;  // copy succeeded — treat as fresh
    memcpy(&next.data[next.data_count++], &d, sizeof(d));
}
// records not matched by manifest kept as-is (they may reference labels the
// new version dropped — they stay DIRTY/ARCHIVED until ARC or explicit erase).
```

**Failure modes:**
- Shrink → immediate reject, no side effects.
- Copy write fails → abort before commit, old record preserved.
- No manifest data section (old-style install) → loop is a no-op; fresh
  records start PRISTINE.

### 4.2 Manifest shape

`meta_install_model.h:40-50` — add:

```c
#define META_MANIFEST_DATA_MAX 8
typedef struct {
    uint32_t play_id;
    uint32_t size;
    char     label[META_DATA_LABEL_MAX + 1];
} meta_manifest_data_t;

// existing meta_install_manifest_t gains:
uint8_t  data_count;                           // §7 L (0..8)
meta_manifest_data_t data[META_MANIFEST_DATA_MAX];
```

Phone-side: `dynslot-pool.js` analyze already emits
`child.partitions[]` (the raw partition list from the embedded table). Add
a helper `buildManifestData(play)` that maps each declared child data
partition to a `{play_id, size, label}` entry (using `play.revisionId` as
play_id).

## 5. Touch point 2: DIRTY marking

**Status**: implemented as the finalize-path marking decided below; no
launch-time hook was added. `meta_carve_flash_set_dirty` is called from
`finalize_locked` (`meta_store_install.c:666`). See §1.2.

`main.c:1306-1320` — after `meta_store_mark_factory_valid` and
`meta_carve_flash_sync_states`, scan the slot table for the slot that the
launcher is about to boot (if any). When `meta_install_session_open` is
called for a VALID slot, add:

```c
// M5 DIRTY mark: the play is about to run — flip its data records from
// PRISTINE to DIRTY so ARC doesn't reclaim user content.
const meta_carve_t *carve = meta_carve_flash_carve();
if (carve) {
    for (uint8_t i = 0; i < carve->data_count; i++) {
        if (carve->data[i].state == META_DATA_PRISTINE &&
            carve->data[i].play_id != 0) {
            // Mark all PRISTINE data records for this play_id as DIRTY.
            // Best-effort: if the write fails (power loss mid-sector),
            // the record stays PRISTINE and gets re-marked on next boot.
            meta_carve_flash_set_dirty(carve->data[i].play_id);
        }
    }
}
```

Alternatively (simpler, less eager): mark dirty at `meta_install_finalize`
success (after esp_ota_end writes the app image). At that point the launcher
knows the install succeeded; the next boot will see the data clean. This
avoids double-marking and is sufficient. Pick the finalize path (see
`meta_store_install.c:667 finalize_locked`).

**Decision**: mark at finalize success (one call, no double-trigger risk).
Rationale: DIRTY is an optimization to protect archives; marking at boot
time risks marking a PRISTINE record that hasn't been populated yet
(install just wrote the app image, data partition is empty at that moment).

Wait — correction: the data carve exists before the app image runs; the
launcher just needs to signal "this play has been launched". The final
successful finalize is the natural trigger: after `esp_ota_end`, after the
flash write succeeds, we know the play is installed. Mark data records as
DIRTY then. The launcher reads the slot's `play_id` from the manifest
already in flight.

Actual code to add in `meta_store_install.c:finalize_locked` after
`esp_ota_end` succeeds (post ~line 690):

```c
// M5: finalizing a successful install — mark this play's data records
// DIRTY (it will be launched, so ARC must not reclaim its content).
if (s_session.manifest.play_id != 0) {
    meta_carve_flash_set_dirty(s_session.manifest.play_id);
}
```

`meta_carve_flash_set_dirty` scans the store's `data[]` for matching
`play_id`, flips `PRISTINE→DIRTY`, re-computes CRC, re-commits (A/B
rotate). Idempotent.

## 6. Touch point 3: uninstall / remove (ARCHIVE)

**Status**: implemented as `meta_carve_flash_archive_slot_and_data` (flip and
commit), called from `h_install_remove` before `meta_carve_flash_remove`; the
two remain separate calls rather than one merged function. See §1.2.

Current `meta_carve_flash_remove(int slot)` (line 167) just removes the
slot record — data records become orphaned (play_id ≠ 0 still in store but
unreachable). Replace with:

```c
// M5: remove slot AND archive matching data records.
esp_err_t meta_carve_flash_archive_slot_and_data(int slot)
{
    const meta_carve_t *cur = meta_carve_flash_carve();
    if (!cur || slot < 0 || slot >= (int)cur->count)
        return ESP_ERR_INVALID_ARG;
    if (cur->slot[slot].kind != META_CARVE_KIND_APP)
        return ESP_ERR_NOT_SUPPORTED;

    // Find play_id of slot being removed.
    uint32_t pid = cur->slot[slot].play_id;

    meta_carve_t next = *cur;
    // First, archive all matching data records (oldest-first, per L1 of
    // reclaim ladder).
    for (int8_t j = (int8_t)next.data_count - 1; j >= 0; j--) {
        if (next.data[j].play_id != pid) continue;
        if (next.data[j].state == META_DATA_DIRTY ||
            next.data[j].state == META_DATA_PRISTINE) {
            // ARCHIVED = user data preserved; erase bytes (lazy erase —
            // leave bytes in flash until ARC actually reclaims).
            next.data[j].state = META_DATA_ARCHIVED;
            // Note: we do NOT erase bytes here; we only flip state.
            // Erase happens at ARC time or on explicit user erase.
        }
    }
    // Then remove the slot.
    if (!meta_carve_remove(&next, (uint8_t)slot))
        return ESP_ERR_INVALID_ARG;

    return meta_carve_flash_commit(&next, true);
}
```

Call from `meta_install_model_remove_ok` (or wherever the remove path
lives today) instead of `meta_carve_flash_remove`.

**Archive policy**: default always ARCHIVE (user data preserved). Explicit
erase (UI choice) calls `meta_carve_flash_erase_data(pid, label)` which
erases bytes and removes the record. This is a separate code path from
the default archive.

## 7. Pool-pressure ARC (reclaim ladder tier 3)

When `meta_carve_place` fails and `meta_carve_free(c) < needed`, call
ARC:

```c
// Reclaim whole ARCHIVED data records, oldest-first, until at least 'target'
// bytes are freed. Returns the number of bytes actually reclaimed
// (0 = nothing to reclaim, or the commit failed).
// Only ARCHIVED records are eligible; DIRTY (live) records are never touched.
// PRISTINE content is tier 4 (last resort, needs user consent) and is not
// reclaimed here.
uint32_t meta_carve_flash_arc(uint32_t target);
```

**Behaviour** (pinned by `tests/test_meta_carve_flash.c::test_arc`):

- Records are selected oldest-first (array order = allocation order).
- Whole-record reclaim keeps every erase range 4 KB aligned, which
  `esp_flash_erase_region` requires; it may overshoot `target`, and
  over-reclaiming is the safe direction.
- The updated record is committed **before** the bytes are erased, so a power
  loss can at worst leave an unreferenced hole, never a record that points at
  erased bytes. A failed commit returns 0 and erases nothing.

**Caller**: `meta_install_model_slot_fit` or `meta_install_model_default_slot`
— where the no-fit decision is made, before returning `ESP_ERR_NO_MEM` /
`meta_install_geom_t.fit = false`. Add a second attempt after ARC.

**UI integration**: when ARC exhausts and still no-fit, report
`no-fit; largest_gap=X; archive=Y bytes available` so UI can suggest
"archive recordings from X first".

## 8. Public API to add

All in `meta_carve_flash.h` (or a new `meta_carve_lifecycle.h` if scope
grows — but keep in flash module for now):

```c
// M5 — DIRTY marking (idempotent; no-op if already DIRTY).
// Called at finalize success (post esp_ota_end).
esp_err_t meta_carve_flash_set_dirty(uint32_t play_id);

// M5 — archive all data records for a play_id (on uninstall).
// Does NOT erase bytes; only flips state + re-commits.
esp_err_t meta_carve_flash_archive_slot_and_data(int slot);

// M5 — erase bytes + remove specific data record (explicit user erase).
esp_err_t meta_carve_flash_erase_data(uint32_t play_id, const char *label);

// M5 — pool-pressure ARC; reclaims whole ARCHIVED records until >= target
// bytes are freed. Returns the reclaimed byte count (0 = none).
uint32_t meta_carve_flash_arc(uint32_t target);

// Internal: copy bytes from src→dst in pool (used by upgrade migration).
esp_err_t meta_carve_flash_data_copy(uint32_t src, uint32_t size,
                                     uint32_t dst);
```

## 9. Test plan

Host tests in `tests/test_meta_carve_flash.c` (RAM NOR model); the M5 cases
assert state flips survive a simulated restart:

| Test | What it covers |
|---|---|
| `test_set_dirty_pristine` | PRISTINE→DIRTY at finalize, idempotent |
| `test_set_dirty_already_dirty` | Already DIRTY: no-op |
| `test_archive_slot_and_data` | Slot removal archives matching data, preserves others |
| `test_upgrade_copy_same_size` | In-place copy, state stays PRISTINE post-copy |
| `test_upgrade_copy_grow` | Grow copy, old offset freed in next ARC pass |
| `test_upgrade_shrink_reject` | Shrink → `ESP_ERR_NOT_SUPPORTED`, no side effect |
| `test_arc_archived_records` | Oldest first, whole-record reclaim, commit before erase |
| `test_arc_exhausted` | All ARCHIVED exhausted → returns 0 |
| `test_arc_preserves_dirty` | DIRTY records untouched by ARC |
| `test_data_copy` | Destination erased before upgrade copy (NOR AND model) |
| `test_v1_orphan_data_ignored` | play_id=0 records skipped by all lifecycle ops |

Golden fixtures: reuse `tests/fixtures/carve_migration_table.bin` plus an
extended store snapshot with 4 data records in mixed states (PRISTINE/
DIRTY/ARCHIVED).

## 10. Edge cases and boundaries

- **V1 orphan data** (`play_id=0`): skip all lifecycle actions. They
  survive in the store forever (or until ARC, which currently skips them
  too). Future: add "orphan cleanup" UI option.
- **Multi-play label sharing (M4)**: two plays declaring the same
  `(label, type)` map to the same physical partition. Each gets its own
  data record with its own `play_id`. ARC on one play erases its bytes —
  this **breaks** the other play's data. Guard: when two records share
  `(label, type)`, ARC on one requires the other to also be ARCHIVED (or
  PRISTINE) — otherwise reject ARC and tell the UI. Implementation:
  `meta_carve_data_label_reserved` already catches exact-reserved labels;
  extend it to accept "shared" labels from the manifest and gate ARC.
- **Power loss mid-copy** (upgrade): bytes may be half-copied. On next
  boot, `meta_carve_flash_ensure` verifies store CRC; if torn, older A/B
  wins. The old offset still has content; the new offset has partial
  content. On next boot, the data record is PRISTINE; the next install
  re-attempts copy. Acceptable.
- **Power loss mid-state-flip** (ARC, archive, set_dirty): A/B rotate
  protects the store (same crash story as §4.7 "mid store commit"). Flash
  erase (ARC bytes) is sector-granularity; torn erase leaves sector in
  erased state, which is the safe baseline (bytes read as 0xFF — FS
  format on next access will recover or fail cleanly).
- **Signature-bound PRISTINE** (`meta_sign_sha256`): PRISTINE data
  partitions may carry pre-populated content (e.g., bundled ROM packs for
  GameBoy `roms`). After a DIRTY transition (play launch), the record's
  sha256 is no longer valid. Design decision: **do not store sha256 on
  data records**. PRISTINE content is trusted by virtue of being in the
  pool at the carved offset; verification only happens at install time
  (when the phone re-sends the content to populate the data partition).
  After that, the record's state transitions are sufficient trust signals.

## 11. Open decisions (pre-implementation review)

1. **When to mark DIRTY**: at `finalize_locked` success (after `esp_ota_end`)
   vs. at `app_main` boot scan of the selected slot. Finalize path is
   cleaner (single call, post-install confirmation); boot scan is more
   robust against a crash between finalize and first boot. **Recommend
   finalize path** — DIRTY is an ARC-optimization, not a correctness
   gate; stale DIRTY means slightly wasted archive space, not data loss.
2. **Explicit erase vs. default archive on uninstall**: default ARCHIVE
   is safer (user data survives) but accumulates pool debt. Explicit
   erase (user-toggled in UI) should be the opt-out. **Recommend
   default ARCHIVE with UI confirm.**
3. **M4 shared-label ARC guard**: enforce when two active records share
   a label (not just reserved system labels). Add to `meta_carve_valid`
   or `meta_carve_flash_arc`. **Recommend yes** — otherwise ARC silently
   corrupts another play's data.

## 12. Import Failure Handling (Strict Firmware Version Matching)

Meta-pass is a data carrier, not a data interpreter — the metadata layer
does not understand child firmware data formats. Therefore, **no format
compatibility checks are provided**; instead, **restored data must match
the firmware version exactly**.

### 12.1 Backup File Header Structure

Write `firmware_version` (play's full version string, e.g. "1.2.3") when exporting:
```c
typedef struct {
    uint32_t magic;           // "MPTB" (Meta-Pass Tape Backup)
    uint32_t version;         // Backup format version (currently 1)
    uint32_t play_id;         // Play ID
    char     firmware_version[32];  // Firmware version string (key!)
    uint32_t data_count;
    // ... data record list
} meta_backup_header_t;
```

### 12.2 Import Validation Logic

```
User clicks "Import Backup"
  → Read backup file header
  → Get current firmware version on device (from manifest or installed firmware)
  → Compare firmware_version
  → If match: normal import
  → If mismatch: reject, notify specific version difference
```

### 12.3 Failure Scenarios & User Notifications

| Failure Scenario | Error Code | User Notification (Example) |
|---|---|---|
| Firmware version mismatch | `ERR_FIRMWARE_VERSION_MISMATCH` | "Backup data is from play v1.2.3, but current device runs v1.3.0. Please ensure operations on the same version device, or contact play author for cross-version migration tool." |
| Checksum failure | `ERR_DATA_CORRUPT` | "Backup data is corrupted and cannot be restored. Please re-export or contact the play author." |
| **Insufficient space after import** (ARC reclaimed but still not enough) | `ERR_POOL_INSUFFICIENT` | "Device space insufficient. Please **delete some plays** on device to free space, then retry import." |

### 12.4 Space Insufficient Handling Flow

When import requires additional space and ARC reclamation is still insufficient:
1. Calculate gap = import data size - current free space
2. Prompt user to delete some plays (show list of deletable plays and their space usage)
3. After user deletes, recalculate space
4. If still insufficient, prompt again (may need to delete more plays)

**Key principle**: Clearly tell user "how much space needs to be freed" rather than vague "insufficient space" message.

### 12.5 Why No Format Compatibility Checks?

1. **Meta-pass doesn't understand data semantics**: recordings, saves, custom binaries — format parsing is the play author's responsibility
2. **Avoid metadata layer over-design**: format_version, label existence checks all require meta-pass to understand play internals, increasing maintenance cost
3. **Clear responsibility boundary**: play authors handle data format compatibility; meta-pass only handles secure data transfer

### 12.6 Cross-Version Recovery Path

If play authors truly need to support cross-version data recovery:
- Play authors provide **dedicated migration tools** (e.g., PC-side scripts, in-play buttons)
- Meta-pass only provides raw data transfer, no format conversion participation

**Prohibited behaviors**:
- Silently skip incompatible data (user unaware of loss)
- Vague error messages (e.g., "Import failed" without reason)
- Auto-attempt format conversion (may cause data corruption)
