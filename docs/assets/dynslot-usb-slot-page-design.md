<p align="right">
  <a href="dynslot-usb-slot-page-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>
# USB Installer Slot Page: Static Slots → Dynamic (dynslot) Space

Date: 2026-10-05
Branch: `main`
Parent: `dynslot-design.md` §4.5 (L4, USB side), §4.7
Related: `dynslot-usb-installer-design.md` (launcher-upgrade dual mode — separate
concern, already designed; this document covers **install + slot management** on
the same page)

## 1. Problem

The USB serial installer (`install-slot/install-slot.html`) still models space as
three fixed slots:

1. **Step 2 is a hard-coded 3-radio group** (`Slot 0 @0x180000 / Slot 1
   @0x360000 / Slot 2 @0x560000`). `applyDiscoveredSlots()` rebuilds the radios
   from the device partition table (`discoverSlots`), so *geometry* is dynamic,
   but the page still has no idea which slots hold a play, which are empty, how
   much pool space is free, or how many slots exist at all beyond what the table
   declares.
2. **No slot removal.** The device side has `GET /api/install/slots` +
   `POST /api/install/remove` (LAN channel). Over USB the user cannot free a
   slot at all, so a full pool can only be recovered through the phone.
3. **No allocation.** Writing an app image at a *new* pool offset is worthless
   unless the carve record in `store` (0x35A000) also gains a slot entry —
   otherwise the launcher never sees the play (orphan bytes; the next install
   may allocate the same offset and overwrite them). Today the page can only
   write at table-discovered `ota_N` offsets, i.e. it never creates space.
4. **No UI feedback** for space: no used/free/largest-gap numbers, no "a new
   slot will be created at 0x…".

Note the launcher-upgrade step (step 7) already probes dynslot vs fixed layout
(`isDynslotLayout`, `comparePartitionTables(.., isDynslot)`); the **install and
slot-management UI did not follow**.

## 2. Goals / non-goals

**Goals**

- G1 On a dynslot device the page renders the *real* slot list (count, offset,
  size, state, name, kind) from the carve record, plus pool space accounting.
- G2 The user can **remove a slot from the page**: bytes of that slot are
  invalidated, the slot entry leaves the record, the 0x8000 table is
  re-materialized without it, and the row disappears with the freed bytes
  reflected in the summary.
- G3 Installing a play **allocates space dynamically**: reuse an existing empty
  APP slot that fits (device semantics `meta_carve_find_fit`), otherwise carve a
  new slot first-fit (`meta_carve_place` / `dynslot-pool.js:carvePlace`), write
  the record, materialize the table, then stream the image — and the UI shows
  the plan (target row, offset/size) before the write and the result after.
- G4 Every number the user sees and every action the UI offers come from **one
  decision source** (BUG-20): a single pure view model computed from the record
  + the loaded image.
- G5 Fixed-slot / factory devices keep today's behaviour (legacy 3-radio view),
  with an explicit mode indicator so "legacy fallback" is never mistaken for
  "dynamic probe failed".
- G6 Local, hardware-free verification: a mock device mode (`?mock=1`) drives
  the very same page code and codec against an in-memory flash, so interaction
  can be tested in a browser and by humans before touching a device.

**Non-goals**

- N1 No device-firmware change. Everything here is page-side; the on-device
  contract (record format, allocator, hook repair) is consumed as-is.
- N2 No data-carve (M5) management over USB: data records are left untouched by
  a slot removal; they keep occupying their own extents (shown separately in
  the summary). Erasing a play's data partitions over USB is a follow-up.
- N3 No compaction/defrag (L1) and no change to the LAN/phone channel.
- N4 No whole-region byte erase at removal time beyond invalidating the image
  header — see §7.2 for why this matches the device's own remove path.

## 3. Data sources and mode detection

On connect (after `loader` is ready) the page reads:

| # | Source | Address | Cost |
|---|---|---|---|
| S1 | live partition table | 0x8000, 0xC00 (read 0x1000) | 1 frame |
| S2 | carve record A/B | 0x35A000 / 0x35B000, 0x1000 each | 2 frames |

Detection order (first match wins):

1. **`DYN_SLOT`** — record A or B decodes (`MPSC` + version + CRC + structure).
   Model = the record. Table only used for cross-check/diagnostics.
2. **`DYN_FRESH`** — no valid record, but S1 contains `store @0x35A000`
   (`isDynslotLayout`). Model = empty carve (0 slots); every install carves.
3. **`LEGACY_FIXED`** — no record, S1 has `ota_0/1/2`. Model = table-derived
   slots (today's `discoverSlots` behaviour); no record writes ever (0x35A000 is
   unallocated gap in this layout — writing there is forbidden).
4. **`LEGACY_FALLBACK`** — blank/unparseable table. Keep current legacy 3-slot
   fallback + explicit warning text (unchanged).

Read failures are non-fatal: the page keeps the previous mode and says so
(`slot_geo_*` strings), never silently pretends to be dynamic.

**Write rule:** record writes happen **only** in `DYN_SLOT` / `DYN_FRESH`.

## 4. Carve record codec in JS — `install-slot/dynslot-record.js`

New zero-dependency ES module (page + Node tests share it), mirroring
`main/meta_carve_store.c` / `main/meta_carve.c` byte for byte.

Record layout (v2, 4 KB sector; full field table in `meta_carve_store.h`):

```
[0,16)      "MPSC" | version u16=2 | slot_count u16 | seq u32 | data_count u16 | 0xFFFF
[16,752)    slot[8] × 92B: state u8 | kind u8 | 0xFFFF u16 | offset u32 | size u32 |
            image_len u32 | play_id u32 | sha256[32] | name[40] (0 padded)
[752,3824)  embedded carved partition table (0xC00, own MD5 marker)
[3824,4080) data[8] × 32B: play_id u32 | offset u32 | size u32 | state u8 | subtype u8 |
            type u8 | 0xFF | label[16]
[4080,4084) crc32 over [0,4080)   (standard reflected CRC-32, zlib-compatible)
[4084,4096) 0xFF
```

API:

- `decodeRecord(raw4096)` → `{seq, slots[], data[]}` | `null` — magic, version
  (2; v1 read-compatible layout), count ≤ 8, seq ∉ {0, 0xFFFFFFFF}, CRC, field
  ranges.
- `pickRecord(rawA, rawB)` → `{rec, fromA, targetSector}` — newest seq wins
  (signed wrap-around compare, same as device); `targetSector` = the *other*
  sector (device `meta_carve_flash_commit` rotation), **except** when sector A
  holds an `MPCK` Wi-Fi credential backup (see §9 E1) → target B.
- `encodeRecord({seq, slots, data})` → 4096 bytes — validates with
  `carveValid()` first (same rules as C: count/order/alignment/granule/pool/
  overlap/kind/state, data label + reserved-label + printable-ASCII rules),
  materializes the embedded table, CRC last.
- `materializeTable(carve)` → 0xC00 — fixed entries
  (`nvs/phy_init/factory/cardid/store/otadata`, `meta_carve.c:FIXED`) + slot
  entries (`ota_<i>`, subtype `0x10+i`) + data entries, merged by ascending
  offset, then `0xAA50` entries + `0xEBEB` marker + 14×0xFF + MD5(entries).
- `crc32(u8)`, `carveFree`, `carveLargestGap` (thin wrappers over
  `dynslot-pool.js` where the geometry already exists).

**Parity strategy (NO GUESSING rule):** the JS codec is only trusted together
with byte-exact tests against C/gen_esp32part output (§11 T1–T3).

> 2026-10-05 note: `meta_carve_store.h`'s trailing comments claimed
> `/* 3776 */` / `/* 4032 */` while the macros compute 3824 / 4080 — the first
> draft of this codec followed the comment and failed every golden compare.
> Fixed (comment + `_Static_assert` pins) as part of this work.

## 5. View model — one decision source (G4)

`buildSlotView({carve, imageLen, mode})` returns everything the renderer and
the action handlers consume:

```js
{
  mode,                       // DYN_SLOT | DYN_FRESH | LEGACY_* (§3)
  rows: [ { id, slot, offset, size, limit, state, name, kind,
            occupied, targetable, recommended, removeEnabled } ],
  auto:  { enabled, offset, size, limit, reason },      // new-slot proposal
  summary: { count, usedSlots, usedData, free, total, largestGap },
}
```

- `targetable`: APP slots only (`kind === app`) and `imageLen <= limit`
  (`limit = size - 0x1000`, `appLimit()` in `dynslot-pool.js`).
- `recommended`: the first EMPTY targetable row, else `auto` when it is
  enabled (mirrors device `suggestedSlot`: occupied slots are never
  recommended — the device stopped suggesting them in d4209a0 as well). In
  dyn modes, when neither exists the page selects nothing and Install
  answers with `err_pick_slot_first`, so an occupied slot is only ever
  written when the user explicitly picks it — which then asks for overwrite
  confirmation (§6). Removing a slot voids any explicit pick (row indices
  shift after the splice), so a stale index can never retarget a different
  slot. Legacy modes keep the old "first fitting row pre-checked" default.
- `auto` is computed with `carveNeed(imageLen)` + `carvePlace(occupancy, need)`
  over `slots ∪ data` (P1-4 occupancy domain); when no gap fits, `enabled=false`
  and `reason` carries `need`/`largestGap`/`totalFree` for the error line
  (design L1: no defrag, explain instead).
- In legacy modes `rows` come from `discoverSlots()` and `auto` is disabled.
- The remove button, the radio rows and the summary all read the same `rows`.

## 6. Install flow (G3)

Target resolution when the user clicks Install:

```
1. model  = current view (re-read record if stale)
2. target = selected row
     auto      → plan = carvePlace(occupancy, carveNeed(imageLen))
                 if none → error with need / largestGap / totalFree
     slot row  → limit check (imageLen <= slot.size - 0x1000)
                 occupied VALID slot → confirm() naming name/offset/size;
                 cancel → "Install cancelled", zero writes
3. geometry step (only when target is a NEW slot):
     a. encode record with the new slot inserted, state=EMPTY, seq+1 → write
        target sector (4 KB, erase-before-write) → read back + decode + validate
     b. write the record's embedded table to 0x8000 (erase 4 KB + write 0xC00)
        → read back and byte-compare  (record first, table second — same order
        as meta_carve_flash_commit; §9 E3 covers a torn write)
4. payload step (existing code paths unchanged):
     wipeSlotResidue(target) → writeFlash(image) → tail sector (MSIG/MAEG/MNAM)
5. metadata step: commit record again with state=VALID, image_len, sha256,
   name (mirror of meta_carve_flash_set_valid); geometry unchanged → table not
   rewritten; play_id is carried over untouched (0 for USB-installed plays).
6. re-read record → rebuild view → log: "Slot N created @0x… · size … ·
   free …"  or  "Slot N overwritten · free unchanged".
```

Why record-first (step 3 before 4): power loss after step 3 leaves a visible
**EMPTY** slot the user can delete; power loss after 4 without a record entry
would leave orphan bytes that no reader can see (and that the next allocation
may silently overwrite). This matches the device's own ordering ("a new slot
materializes before upload").

Overwriting an existing slot skips step 3 entirely (no geometry change) and
performs a single metadata commit in step 5. Overwriting is always a
deliberate act: occupied slots never receive the default selection (§5), and
picking one pops the confirmation above before any write happens.

## 7. Remove flow (G2)

Two-step confirm (button → `confirm()` naming slot, offset, size, name), then:

```
1. erase the slot's first 4 KB (image header) by writing 0xFF
   → the scan can never resurrect it as VALID (device's own guard against
     "delete resurrects", meta_store_install.c h_install_remove)
2. encode record WITHOUT the slot, seq+1 → write target sector → read back
3. write the record's embedded table to 0x8000 → read back + compare
4. re-read record → rebuild view (row gone, free bytes up)
```

Bytes beyond the header are intentionally left in place: `dynslot-design.md`
§4.5 states erasure happens at the next write (the reinstall path already
wipes residue before writing), and the space is *allocated* again only through
the record, which no longer references it. `eraseData`-style data-partition
wipes are out of scope (N2).

Order rationale: erase-then-commit mirrors the device (erase image header →
`meta_carve_flash_remove` → reply 200). A crash between 1 and 2 leaves an EMPTY
slot (harmless, deletable); a crash between 2 and 3 leaves the record ahead of
the table, which the bootloader hook repairs at next boot (§9 E3).

## 8. UI changes (`install-slot.html`)

- **Step 2 becomes a slot table** (still `data-i18n="step_slot"`):

  ```
  Mode: dynslot (dynamic slots)                       [badge]
  ┌────┬─────────┬──────────────┬────────┬──────────────┬───────┐
  │ ○  │ Slot 0  │ 0x180000     │ 1.8 MiB│ ● Demo Play  │ Remove│
  │ ○  │ Slot 1  │ 0x360000     │ 2.0 MiB│ (empty)      │ Remove│
  │ ●  │ Auto    │ new @0x560000│ 2.6 MiB│ (new slot)   │  —    │
  └────┴─────────┴──────────────┴────────┴──────────────┴───────┘
  2 slots · used 3.8 MiB (data 64 KiB) · free 2.6 MiB / 6.4 MiB · largest gap 2.6 MiB
  ```

  - One row per record slot; state word from `meta_slot_list_word` wording
    (`(empty)` / name / `(no firmware)` — never "invalid", see `meta_slots.h`).
  - `Auto` row (dynslot only): shows the computed new-slot offset/size once an
    image is loaded; disabled with `needs X / largest gap Y` when nothing fits.
  - `Remove` button per row (dynslot only; hidden in legacy modes).
  - Summary line + mode badge: `dynslot (dynamic slots)` / `fixed 3-slot
    (legacy)` / `factory fallback`.
- **Step 4** keeps its button; the status line additionally reports the
  allocation outcome (created vs reused) so the dynamic decision is visible.
- **Step 5 backup checkboxes** continue to be rebuilt from the same model.
- i18n: all new strings added to **both** `I18N.en` and `I18N.zh`
  (`tools/install-slot/test-extract.mjs` enforces key parity + `t()` coverage).
- After any successful install/remove the view is rebuilt **without
  reconnecting** (record is re-read from flash).

## 9. Edge cases and safety

| # | Case | Handling |
|---|---|---|
| E1 | Sector A still holds a legacy `MPCK` Wi-Fi credential backup | Never overwrite it: `pickRecord` targets sector B while A is not a valid record but starts with `MPCK`. The hook's `load_record` then selects B. |
| E2 | No record at all (`DYN_FRESH`) | Target sector A, seq starts at 1 — same as `meta_carve_flash_commit` on a fresh device. |
| E3 | Torn record write / torn table write | CRC rejects the torn sector, older A/B copy wins; table repair is the bootloader hook's job (B5) — page order (record → table) keeps the hook's premise true. |
| E4 | Table read-back mismatch | Abort with layer-tagged error (`table: read-back mismatch`); the record already stands and the hook will materialize it at next boot, so no state is left ambiguous. |
| E5 | Fixed/factory device | No record reads/writes at all; legacy UI (G5). |
| E6 | Record says slots, table disagrees (stale mid-state) | Record wins for the model; step 3/7 re-materialize the table on the next write. |
| E7 | Storage-kind slots (`kind=storage`) | Shown, `targetable=false`, removable (freeing the reservation is the point of L2). |
| E8 | Pool full / fragmented | `auto.enabled=false` + `needs/largestGap/totalFree` (L1: explain, never silently defrag). |
| E9 | Disconnect mid-operation | Existing `markDisconnected` path; every step is idempotent-safe because writes are re-derived from a fresh record read at retry. |

## 10. Mock device mode (G6)

`?mock=1` (also `#mock` for static hosting) switches the page into a simulated
device:

- New module `install-slot/mock-device.js`: 8 MB in-memory flash, seeded with
  a *carved* table + a valid record built by `dynslot-record.js` (2 slots: one
  VALID named play, one EMPTY, plus one data record) and esptool-compatible
  semantics for `readFlash(offset, size, progress)` / `writeFlash({fileArray,
  reportProgress})` (sector-erase-then-write, exactly what the ROM/stub does).
- `sync/connect/runStub/eraseFlash` are stubs so every existing code path
  (including read recovery) runs unchanged.
- Connect skips `navigator.serial` entirely and reports chip
  `MOCK ESP32-C3`; a persistent banner marks the page as simulated so a mock
  session can never be mistaken for a device session.
- Because the mock stores bytes through the *same* codec the production flow
  uses, a browser walkthrough (connect → list → delete → install → list) is a
  genuine byte-level round trip, not a UI stub.
- Node can import the same module, so the interaction sequence is also covered
  by host tests (§11 T6).

## 11. Test plan

| # | Test | Tool |
|---|---|---|
| T1 | `materializeTable` == `carve_migration_table.bin` / `carve_shrunk_table.bin` / `safe_table.bin` (gen_esp32part byte authority; same goldens `tests/test_meta_carve.c` uses) | `tools/install-slot/test-dynslot-record.mjs` |
| T2 | decode(C-generated golden record) → re-encode → **byte-identical**; geometry/name/sha/data assertions pinned | same |
| T3 | CRC golden (`"123456789"` → `0xCBF43926`), reject: bad magic / bad version / CRC tamper / count>8 / bad state | same |
| T4 | `pickRecord`: seq newest-wins, wrap-around, A/B rotation, `MPCK` guard | same |
| T5 | `buildSlotView`: reuse-first recommendation, auto proposal, disabled-auto numbers, storage rows, legacy mode rows, summary math | same |
| T6 | mock device interaction: seed → install (new slot) → record/table/bytes consistent → remove → row+space restored | same |
| T7 | i18n key parity / `t()` coverage for new strings | `tools/install-slot/test-extract.mjs` (existing) |
| T8 | browser walkthrough in mock mode (list, delete, install, summary, banner) | local server + browser, screenshots |
| T9 | real device: dynslot install → launcher lists play; remove → play gone + free grows; fixed-slot device unchanged | manual checklist (§12) |

Gate: `./tools/validate.sh --static` must stay green (host tests + repo checks).

## 12. Real-device verification checklist

1. **Dynslot device, dynamic list**: hold UP, power on, Connect → step 2 shows
   `dynslot` badge, rows equal the plays visible on the device screen
   (offsets/sizes differ per device), summary free bytes match the launcher's
   free-space row.
2. **Dynamic allocation**: pick a local `.bin` larger than every empty slot's
   limit (or fill the pool first) → `Auto` row shows the planned offset; Install
   → log shows `record: seq N→N+1`, `table: materialized`, image write
   progress; reboot → launcher lists the new play at that offset.
3. **Remove**: press Remove on a play row → confirm → row disappears, free
   grows by that slot's size; reboot → launcher no longer lists it; installing
   another play now lands in the freed gap (verify with log offset).
4. **Legacy fixed device**: page keeps `fixed 3-slot (legacy)` badge, 3 radios,
   no Remove buttons; install behaves exactly as before.
5. **Power-loss drill (optional)**: unplug during the record write → page
   reports the write failure; power back on → device boots (hook repair) and
   the slot list matches one of the two A/B states.

Report fields required at delivery (per AGENTS.md):

```
Build:        PASS / FAIL / NOT RUN
Host tests:   PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified:   <remaining checks>
```

## 13. Implementation order

1. `install-slot/dynslot-record.js` (codec + planner + view model) — pure.
2. Golden fixture generation (C side) + `tools/install-slot/test-dynslot-record.mjs`,
   registered in `tools/validate.sh`.
3. Page wiring: mode detection on connect, slot table UI, remove action,
   install target resolution, i18n.
4. `install-slot/mock-device.js` + `?mock=1` wiring.
5. `./tools/validate.sh --static`, then browser walkthrough (T8) and the real
   device checklist (T9).
6. Iterate on human feedback; record user-visible changes in
   `docs/CHANGELOG.md`.

## 14. Open questions

- **Q1**: deep-clean option ("also erase the whole slot") — deferred; today's
  behaviour matches the device remove path and the reclaim ladder (§7.2).
  Add a checkbox if field feedback shows stale bytes confusing users.
- **Q2**: removing a play's *data* partitions from the USB page (N2) — needs a
  `play_id` story for USB-installed plays (recorded as 0 today).
