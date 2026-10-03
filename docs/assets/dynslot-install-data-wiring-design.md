<p align="right">
  <a href="dynslot-install-data-wiring-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# dynslot install-path data-carve wiring (P0-5 draft)

Date: 2026-10-03
Branch: `feat/dynslot`
Parent: `dynslot-m5-design.md` (lifecycle), `dynslot-design.md` §4.5 (reclaim ladder).

Status: **decisions confirmed (2026-10-03)**, revised after review (§1.0/§4/§7);
implement per §1.0/§2/§3/§5; order **P1-4 then P0-5** (P1-4 landed).

## 1. Problem

Two gaps, the first more fundamental than the second:

**F1 (blocking) — the install path never materializes a new slot.**
`meta_install_model_carve_ok` / `meta_install_geom_from_carve` have zero
production callers; `geom_refresh()` derives limits from the live partition
table only and never reads the proposal; the chunk-write path resolves the
partition by subtype, so a fresh device (safe table, zero ota_N entries)
fails at partition lookup on the very first install. P0-5 only fixes data
records, and the transaction would have nothing to attach to.

**F2 — nobody creates data records.** `meta_carve_place_data` and
`meta_carve_largest_gap` have no production caller (`meta_carve_reclaimable`
has one, backup-import logging). The M5 state machine never starts — the
reclaim ladder (tiers 3–5) is unreachable from a real install. Evidence:
`grep -rn` over `main/*.c` shows only definitions and tests.

This draft wires both layers: new-slot materialization (F1) + record
creation (F2) in one transaction, and the reclaim ladder into the no-fit
decision.

### 1.0 How F1 materializes (review addendum 2026-10-03 — key constraint)

**Why the table cannot be rewritten mid-upload: the IDF partition-table
cache.** `esp_partition_find_first` (used by both
`meta_store_slot_partition` and `esp_ota_begin`) loads the 0x8000 table into
an SRAM cache on first access; a slot materialized at run time is invisible
to the current boot. Therefore:

- Materialization happens at **prepare (the confirm step)**, followed by a
  **reboot**; the upload runs on the next boot, where the cache is fresh and
  the existing chunk/OTA paths need zero changes.
- The "idempotent retry" branch already in `carve_ok` (proposal slot already
  in the carve → return its index) exists precisely for this: after reboot
  the phone re-sends the same prepare, hits the idempotent branch, and the
  flow continues. That branch having zero production callers is the other
  half of the F1 evidence.
- Phone side already has the matching machinery: `waitDeviceBack` (poll
  status until the service is back after a reboot) and the extracted()
  retry cache — the same rhythm as the post-delete 150 ms reset.

**Prepare-time materialization sequence (phone-picked and device-confirm
alike):**

1. `carve_ok(m, cur)` validates the proposal (size = `meta_carve_need`,
   offset/index match; already-placed → idempotent return, no duplicate).
2. `meta_carve_place` on the `next` copy (slot first).
3. For each `manifest.data[i]`: reserved-label reject → `place_data` +
   `append` (PRISTINE); any failure → whole thing fails (copy discarded, no
   partial carve).
4. `commit(&next, true)` materializes the table → reboot.
5. Phone re-sends prepare → idempotent hit → session/chunk/finalize on the
   existing path.

**Empty-slot lifecycle on cancel/stall (F3):** materializing at prepare
introduces "carved but never written" slots. Rulings:

- Session cancel (`meta_install_cancel`): a slot created by this session is
  removed via `meta_carve_flash_remove` (not marked INVALID — there is no
  image to be "broken"; INVALID means "had an image, now bad"). Existing
  slots keep the current INVALID path.
- Offer overwritten by a new prepare (`offer_and_upload_clear`): if the
  previous offer had materialized a slot, remove it before clearing. The
  session records a `carved_pending` flag (new slot index) for both paths.
- Phone-disconnect stall: the empty slot stays in the carve, appears in the
  remove list, and can be deleted explicitly. It holds pool space only —
  same class as a half-written INVALID slot after a power cut. No automatic
  GC (honesty boundary).

**set_dirty semantics fix (F4):** finalize currently flips ALL PRISTINE
records of the play_id to DIRTY — wrong for records created by THIS
session (the region was just erased, there is no user data; marking it DIRTY
makes tier-4 PRISTINE reclaim unreachable minutes after creation). Ruling:
`set_dirty` skips session-created records; only pre-existing records
preserved by the upgrade path (data_copy sources) are marked DIRTY at
finalize. Implementation: diff the (play_id,label) set "present at prepare
time" vs "present now", or simpler — records created at prepare are marked
directly.

### 1.1 Addressing model (why the offset is transparent)

This is the mechanism the whole wiring rests on, from
`dynslot-data-unification-research.md` §4 (T1/T3):

- **Children address data by partition label, not by offset.** FatFS/SPIFFS
  mount by label (`esp_vfs_fat_spiflash_mount_rw_wl(path, "recordings", …)`),
  and raw partition users call `esp_partition_find_first(TYPE, subtype,
  label)`. The child never sees an offset.
- The lookup reads the **live partition table at 0x8000** — the carved table
  metapass materializes — **not** the child's in-image table.
- So the flow is: the child's in-image table declares `{label, type, subtype,
  size}` → analyze reads it → the device allocates a pool offset and
  materializes an entry with the **same label** at that offset → the child,
  at runtime, resolves its label to metapass's offset.
- **Migration is therefore just a table-entry edit**: copy bytes to the new
  offset (`meta_carve_flash_data_copy`), update the record's `offset`,
  re-materialize the table with the same label — the child sees the new
  location on its next mount. This is why records are keyed by
  `(play_id, label)`, never by offset.
- **Raw fixed-offset children (T3) cannot be remapped.** There is no
  interception point (VFS is a per-registration driver table; cache-MMU
  remapping is unsupported and fights the bootloader). Contract-level warning
  only. The one known offender was metapass's own Wi-Fi credential backup at
  0x35A000, already relocated (L6).
- **Two boundaries**: the default `nvs` partition is a single label and cannot
  be redirected per slot (children must use their own labels); and
  `(label, subtype)` is unique per carve in v1 (M4 sharing is future work).

Consequence for this design: `manifest.data[]` `{label, size}` is exactly the
child's declared requirement, the allocated offset stays device-side, and the
phone needs no offset (only occupancy, see §3).

## 2. Decision 1 — when and where records are created

**Recommendation: at the confirm/commit step, in the same transaction that
materializes the slot.**

The device runs first-fit for each `manifest.data[]` entry via
`meta_carve_place_data` against the *same* `meta_carve_t` that already holds
the newly placed slot, appends a `PRISTINE` record, and commits once.

Rationale:

- One atomic commit with the slot — no half-installed state, no second
  power-loss window.
- No child-firmware cooperation needed; system policy stays at an
  unbypassable layer (`AGENTS.md`).
- The manifest already carries `play_id` / `size` / `label`.

Alternatives rejected:

- Child self-report on first boot: requires untrusted child cooperation.
- Separate commit after upload: two commits, partial-state window.

## 3. Decision 2 — proposal binding (device-side vs phone proposal)

**Recommendation: device-side allocation only; no data proposal from the
phone in v1.**

Order inside the one commit:

1. `meta_carve_place(cur, carve_size, APP, &next)` places the slot.
2. For each `manifest.data[i]`: reject reserved labels
   (`meta_carve_data_label_reserved`), then
   `meta_carve_place_data(&next, size, &off)` and
   `meta_carve_data_append(&next, ...)` with `state=PRISTINE`.
3. If **any** entry fails to fit → the whole confirm fails (atomic, no
   partial carve).
4. `meta_carve_flash_commit(&next, true)`.

Consequence for the phone: the slot proposal in `dynslot-pool.js`
`carvePlace` must mirror the **union** occupancy (slots ∪ data), or a slot
proposal will land inside a data record and the device will reject it (L4
divergence). This makes **P1-4 a prerequisite**: the `/api/install/slots`
listing must expose data records' `offset`/`size` (or a combined occupancy
view) before any data record can exist. Until then, data records cannot be
created without risking L4 divergence.

Data `(label, subtype)` uniqueness across plays is already enforced by
`meta_carve_valid`; M4 sharing remains future work.

## 4. Decision 3 — no-fit rejection shape

Proposed JSON on the prepare/confirm failure (status code **TBD**, see §7):

```json
{
  "reason": "no-fit",
  "for": "slot",
  "needed": 1572864,
  "largestGap": 1048576,
  "reclaimableArchived": 4096,
  "reclaimablePristine": 8192
}
```

- `for`: `"slot"` or `"data"`; when `"data"`, add `"label": "<label>"`.
- `needed`: bytes required (`meta_carve_need(image_len)` for a slot, the
  record size for data).
- `largestGap`: `meta_carve_largest_gap(cur)` — the biggest single
  allocatable extent.
- `reclaimable`: `meta_carve_reclaimable(cur)` — ARCHIVED + PRISTINE bytes
  the ladder could free.

This satisfies design §4.5 tier 5 ("reject with the numbers") and §12.4
("tell the user how much must be freed").

## 5. Reclaim ladder wiring

On allocation failure for the slot or a data entry:

1. **Tier 3** — if `reclaimableArchived > 0`, run
   `meta_carve_flash_arc(needed - free)` (ARCHIVED only, PRISTINE
   untouched), then retry the placement once.
2. **Tier 4** — PRISTINE content is reproducible from the install image but
   may still hold live play data; **do not auto-drop**. Report
   `reclaimablePristine` and require explicit user consent (a future
   `/api/install/reclaim` with a PRISTINE scope, or the existing
   `eraseData` path).
3. **Tier 5** — still no fit → reject with the §4 JSON.

No silent auto-move / compaction (L1 honest boundary stands; compaction is
deferred to v2).

## 6. Test plan

- **Model (host, linkable)**: add `meta_install_model_data_ok(m, cur)` that
  replays the per-entry placement over a `meta_carve_t` copy and returns a
  verdict + the §4 numbers. Cover: fits, no-fit (slot), no-fit (data),
  reserved label, duplicate label, atomic rollback.
- **Flash (host, RAM NOR)**: install-path commit that adds a slot + data in
  one transaction; verify the record persists across a simulated restart and
  the materialized table contains the data entry.
- **JS (Node)**: extend `test_dynslot_pool.mjs` with a union-occupancy case
  once P1-4 lands; verify the phone's slot proposal matches the device
  allocator when a data record sits in the pool.
- **Fixture**: a carve snapshot with one slot + one data record.

## 7. Resolved (2026-10-03, after review)

1. **Status code** for no-fit: `409 Conflict` — the request conflicts with
   the resource's current state (insufficient space is a state problem, not
   a syntax problem); backup import's `507` is a different context, no
   forced consistency. Response body = the §4 JSON.
2. **Order**: slot first (keeps app offsets stable, matches the phone's
   slot proposal).
3. **Per-record breakdown**: report the **first** failed entry only —
   failure aborts the whole transaction (atomic), a full list has no
   decision value.
4. **P1-4 dependency**: landed (58caa0e — `/api/install/slots` exposes data
   records, `parseSlots` passes them through, `geomFromListing` unions
   occupancy); record creation unblocked.
