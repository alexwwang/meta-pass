<p align="right">
  <a href="dynslot-install-data-wiring-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# dynslot install-path data-carve wiring (P0-5 draft)

Date: 2026-10-03
Branch: `feat/dynslot`
Parent: `dynslot-m5-design.md` (lifecycle), `dynslot-design.md` §4.5 (reclaim ladder).

Status: **decisions confirmed (2026-10-03)** — implement per §2/§3/§5; order is
**P1-4 then P0-5** (see §7).

## 1. Problem

`meta_install_model.c` parses the manifest `data[]` array into the session,
but nothing creates data-carve records: `meta_carve_place_data`,
`meta_carve_largest_gap`, and `meta_carve_reclaimable` have no production
caller. The M5 state machine therefore never starts — the reclaim ladder
(tiers 3–5) is unreachable from a real install. Evidence: `grep -rn` over
`main/*.c` shows only definitions and tests.

This draft fixes that by wiring record creation into the install commit and
the reclaim ladder into the no-fit decision.

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
  "reclaimable": 4096
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

1. **Tier 3** — if `reclaimable > 0`, `meta_carve_flash_arc(needed - free)`,
   then retry the placement once.
2. **Tier 4** — PRISTINE content is reproducible from the install image but
   may still hold live play data; **do not auto-drop**. Report
   `reclaimable` and require explicit user consent (a future
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

## 7. Open questions for the reviewer

1. **Status code** for no-fit: `400 Bad Request`, `409 Conflict`, or
   `507 Insufficient Storage`? (Backup import already uses `507`.)
2. **Order**: place the slot before or after data? Slot-first keeps the app
   offset stable and matches the phone's slot proposal; data-first would
   keep recordings contiguous. Recommendation: slot-first.
3. **Per-record breakdown**: is a single failed-entry report enough, or does
   the UI need the full list of what could not fit?
4. **P1-4 dependency**: confirm that exposing data occupancy in
   `/api/install/slots` is in scope before data records are created
   (otherwise defer data creation until P1-4 lands).
