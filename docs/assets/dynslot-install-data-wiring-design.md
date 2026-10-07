<p align="right">
  <a href="dynslot-install-data-wiring-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# dynslot install-path data-carve wiring (P0-5 draft)

Date: 2026-10-03
Branch: `feat/dynslot`
Parent: `dynslot-m5-design.md` (lifecycle), `dynslot-design.md` §4.5 (reclaim ladder).

Status: **final (2026-10-04, implementation addendum)** — revised after review (§1.0/§4/§7) and
arbitrated: (1) the original incorrect P0-5 data_copy migration block is retired, (2) F4 mask-based DIRTY,
(3) plan B materialization (end-of-install reboot + fabricated handles). The current implementation retains
M5's **correct pool-internal DATA resize migration** (erase destination, copy bytes, then switch the durable
carve; rollback preserves the old extent). This is existing-DATA upgrade behavior, not the P0-5 initial DATA
payload path. Implement per §1.0/§2/§3/§5; P1-4 landed, P0-5 device/phone wiring done.

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

### 1.0 How F1 materializes (final 2026-10-04: plan B — end-of-install reboot)

**Hard constraint: the IDF partition-table cache.** `esp_partition_find_first`
(used by both `meta_store_slot_partition` and `esp_ota_begin`) loads the 0x8000
table into an SRAM cache on first access; a slot materialized at run time is
invisible to the current boot. Children and the bootloader read the table on
the NEXT boot and are consistent by construction — so the only open question
is how THIS boot's upload/verify path addresses the carved slot.

**Plan B (arbitrated 2026-10-04): fabricated handles for the upload; the
reboot lands after a successful install.**

1. **Prepare (the confirm step):** `place_offer` decides on a copy (shape
   check → idempotent scan + data backfill → slot first → per-entry data
   placement); OK with changes → `commit(&next, true)` (record + table in one
   transaction). The stale in-boot cache is harmless.
   - No-fit → tier 3: if ARCHIVED bytes exist, one `arc()` reclaim and a
     single retry; still no-fit → **409 + JSON** (needed / largestGap /
     reclaimableArchived / reclaimablePristine [ , label ], §4).
   - With a proposal, the `slot_fit` check is skipped (place_offer is the fit
     authority; a new slot is in neither geom nor cache). The F4 mask
     snapshots pre-existing records BEFORE placement.
2. **Upload:** chunk/finalize resolve the partition by confirmed_slot; on a
   cache miss they fall back to a **carved handle** (address/size/subtype=
   0x10+idx/label=ota_idx, stored statically so no pointer escapes). OTA
   erase/write and `esp_image_verify` consume value fields only and never
   consult the table. Idempotent retries (phone re-sending the same prepare)
   hit the place_offer idempotent branch → no duplicate materialization.
3. **Success:** if this session materialized the table (`table_changed`),
   reboot ~300 ms after the response flushes (esp_timer, same rhythm as the
   remove flow). The device list and the boot path are fully consistent on
   the next boot.
4. **Cancel / reject / overwritten offer:** a **session-created slot** is
   reclaimed via `meta_carve_flash_remove` (no user data inside: empty or a
   half-written garbage image); existing slots keep the INVALID path. A
   `manifest_valid` guard prevents the static zero-initialized
   `carved_new_slot == 0` from deleting slot 0 at boot. On success
   `carved_new_slot` is reset to -1 BEFORE the session clear so the
   just-installed slot can never be reclaimed.

**Rejected alternatives:** reboot right after prepare (an extra phone-device
round-trip to re-send prepare; and the remove flow shows "reboot" is only a
rhythm convention here); zero reboots end-to-end (fabricated handles PLUS
re-deriving the device list scan from the carve record — saving ~3 s in
exchange for two permanent IDF bypasses, poor trade).

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
