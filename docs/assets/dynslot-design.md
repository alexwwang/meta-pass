<p align="right">
  <a href="dynslot-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Dynamic Slot Partitioning (dynslot) Design

Date: 2026-10-02
Branch: `feat/dynslot` (design proposal — no implementation yet)
Evidence basis: `docs/assets/play563-appstore-download-reverse.md`, byte-level
verification of the hosted play 563 binary, ESP-IDF 5.5.3 sources, and the
current meta-pass tree (`main` @ `9e591a2`).

## 1. Problem

The slot geometry is frozen at three fixed partitions of unequal size:

| Slot | Partition | Raw size | Usable (`size − 4KB` tail) |
|---|---|---:|---:|
| 0 | ota_0 @ 0x180000 | 0x1D6000 (1,896,448 B) | 1,921,024 → limits: 1,892,352 |
| 1 | ota_1 @ 0x360000 | 0x200000 (2,097,152 B) | 2,093,056 |
| 2 | ota_2 @ 0x560000 | 0x29E000 (2,720,512 B) | 2,716,416 |

Fit is a per-slot cliff: an image must fit *one* partition, even when the sum
of free space across slots is sufficient. Examples that fail today but fit a
pooled layout (numbers measured, see §2):

- Two apps of ~3.2 MB each (play 563's own factory is a 3 MB partition in its
  embedded table): need 6,447,104 B — impossible today (max single usable
  2,716,416 B), trivial in a 6.45 MiB pool.
- 2,740,224 + 1,844,496 + 1,200,000 B side by side: fails today (2,740,224
  only fits slot 2, then 1,844,496 only fits slot 1), packs fine in a pool.

Other consequences of the fixed table: the geometry is duplicated in three
places that must stay in sync (`partitions.csv`,
`install-slot/store-analyze.js` `SLOT_GEOMETRY`,
`install-slot/install-slot.html` `SLOTS`), and the release gate freezes it via
`UPGRADE_PRESERVED_REGIONS` all-0xFF checks (`tools/verify_firmware.py:34-40`).

Goal: replace fixed slots with a **flash pool carved into variable-sized
slots**, managed by the launcher, enforced by the bootloader hook.

## 2. What play 563 does (verified, and what we borrow)

Byte-level verification of the hosted binary
(`/api/download/community/ai-passport-9`, SHA-256
`2956f77b…3fbdf01`, matches catalog) and ESP-IDF 5.5.3 sources:

1. **The downloaded play carries its own partition table.** The merged image
   at 0x8000 decodes to: nvs 24K, phy_init 4K, factory 3M, otadata 8K,
   cardid 16K, ota_0 3M, store 16K (NVS), easter 388K, recovery 1M.
2. **Table MD5 format** (verified against the binary and
   `gen_esp32part.py:411`): `md5(entries)` placed right after a
   `0xEBEB + 0xFF×14` terminator — *not* the whole sector. A runtime table
   rewrite is therefore a fully specified, reproducible byte format.
3. **The installer builds a range plan from the embedded table and writes
   flash directly** (per the prior disassembly: `store_ota`, 2,048-byte
   chunks, streaming SHA-256, table write retried up to 3×).
4. **A central metadata partition (`store`, 16K NVS) records installs** —
   slot state lives outside the app images.
5. **Correction to the prior reverse doc:** recovery sits at subtype 0x20,
   which the bootloader classifies as "Unknown app", *not* an OTA slot
   (`bootloader_utility.c` tests `(subtype & ~0x0F) == 0x10`; mask is 0x0F,
   `esp_flash_partitions.h:21-22`). Only subtypes 0x10–0x1F — **16 OTA
   slots** — are otadata-selectable.
6. No Range/resume, single direct-origin download — fast path only. Meta-pass
   keeps its Range/resume machinery; not borrowed.

Borrowed mechanisms: **runtime partition-table materialization with the MD5
format**, **central slot metadata outside the images**, **the manager
(launcher) — not the child — owns the layout decision**.

Not borrowed: play 563 replaces the *whole* table with the child's own table,
which would evict the launcher. Meta-pass's launcher must survive every
install, so carve tables are launcher-computed and constrained to the pool.

## 3. Hard constraints (all verified)

- The IDF 2nd-stage bootloader boots only partitions declared in the table at
  0x8000, and verifies its MD5 at every boot
  (`bootloader_utility.c:158` → `esp_partition_table_verify`).
- otadata slot selection is count-agnostic: `slot = (ota_seq−1) % ota_count`
  (`bootloader_utility.c:412-415`), and the existing hook already scans the
  table for the otadata address and slot count at runtime
  (`bootloader_components/meta_boot_hooks/hooks.c:69-72`).
- Protected, immutable: factory @ 0x10000 ≤ 0x170000, cardid @ 0x356000/0x4000
  (`tools/verify_firmware.py:22-24`).
- No secure boot, no flash encryption (`sdkconfig.defaults`) → runtime
  partition-table writes are permitted.
- System-level policy must be enforced at an unbypassable layer
  (AGENTS.md:20) → the enforcement point is the existing bootloader hook, not
  child cooperation.
- App partition offsets must be 64 KB-aligned; sizes a multiple of 4 KB.

## 4. Design

### 4.1 Physical model: pool + carve

The post-cardid app space becomes one logical pool in two physical ranges
(cardid cannot be spanned by a partition):

- pool_0: [0x180000, 0x356000) — 0x1D6000 (1,896,448 B)
- pool_1: [0x360000, 0x7FE000) — 0x49E000 (4,841,472 B)
- **Total pool: 6,766,592 B (6.45 MiB)** — vs 6,754,304 B usable today; the
  gain is allocation flexibility, not capacity (≈45 KB is additionally spent
  on ≤8 tail sectors + the 24 KB store).

The dead 24 KB alignment gap 0x35A000–0x360000 (today an unpartitioned raw
Wi-Fi credential backup, `main/meta_store_net.c:119-120`) becomes the
**store partition** (`store`, data/NVS, 0x35A000/0x6000). The credential
backup relocates inside the store region (§6, L6).

Allocator rules:

- Slot offsets 64 KB-aligned; sizes rounded to 4 KB; **min slot 128 KB**.
- **Max 8 slots** (IDF hard cap is 16 OTA subtypes; 8 is the design cap).
- First-fit across pool_0 then pool_1. The small-segment-first order is
  deliberate: pool_0 (~1.8 MB, cut off by cardid) absorbs small slots first
  so pool_1 keeps its large contiguous span for big apps/archives. No
  compaction in v1 (§6, L1).

### 4.2 The three table states

1. **Safe table** — compiled into the factory image, embedded in the store,
   and shipped in the release artifact at 0x8000: nvs, phy_init, factory,
   cardid, store, otadata, plus the two pool ranges declared as *data*
   partitions (unbootable placeholders). Guarantees a bootable device in the
   worst case; a corrupt carve can never make garbage bootable.
2. **Carved table** — safe entries plus one app entry per live slot
   (ota_0..ota_n) at carve offsets, generated byte-for-byte per the verified
   MD5 format (`md5(entries)` + `0xEBEB 0xFF×14` + digest).
3. **No third state at boot** — the bootloader hook (§4.4) repairs any
   deviation before otadata is honored.

### 4.3 Carve metadata (`store` partition)

A/B records (seq + CRC32, erase-before-write, the pattern already used by
`meta_boot_policy.h`):

```
magic, format_version, seq, slot_count,
slot[i] = { state, offset, size, image_len, image_sha256, name[40] }
crc32
```

Plus a copy of the safe table and a scratch sector: 24 KB total suffices.
Slot states mirror the scan states (EMPTY/VALID/INVALID,
`main/meta_slots.h:14-18`). The launcher re-validates every record against
flash on each boot (SILENT image verify + tail sector, the existing
`meta_store_scan` logic) and garbage-collects dead records — derived state
stays derived; the store is a cache of intent, never blindly trusted.

### 4.4 Boot and validation flow (enforcement layer)

```text
ROM → 2nd-stage bootloader
  │  IDF verifies table MD5 @0x8000
  ▼
meta_boot_hooks (extended)
  │  read store A/B → newest valid seq (CRC ok)
  │  compare live table entries vs committed carve:
  │    match            → proceed
  │    mismatch/corrupt → rewrite 0x8000 from store copy
  │                        (store dead → write compiled-in safe table),
  │                        wipe otadata, boot factory
  ▼
otadata selection (unchanged IDF semantics)
  cold boot ⇒ otadata empty (single-session policy, hooks.c:106-128)
  ⇒ factory / launcher
  ▼
launcher app_main
  table != committed carve ? materialize carve (no-op when unchanged) : —
  scan slots from carved table (esp_partition_find_first, unchanged)
  user picks play:
    carve unchanged → esp_ota_set_boot_partition(ota_i) + restart
    carve changed   → commit store A/B → materialize table → set otadata
                      → restart; hook validates carve before honoring otadata
```

Per carve-affecting install/remove: one extra reboot. Booting a play under an
existing carve: zero extra reboots (same as today).

### 4.5 Install/remove path changes

- **Analyze**: `SLOT_GEOMETRY` is replaced by one pool descriptor
  (`ranges, min_granule, min_slot, max_slots, tail_sector`) in a shared JS
  module consumed by `store-analyze.js`, `phone-install.js`, and
  `install-slot.html` — geometry copies drop from three to one, with the same
  device-authority interlock as today (`meta_install_model_offer_ok` rejects
  any phone-side proposal the device cannot reproduce locally,
  `main/meta_install_model.h:83-84`).
- **Proposal**: phone computes a proposed carve (first-fit, or reuse an empty
  slot); device re-runs the allocator and rejects divergence.
- **Write path unchanged**: extracted app image, `esp_ota_begin/write/end`,
  streaming SHA-256, tail sector with MSIG/MNAM/MAEG
  (`main/meta_store_install.c:466-621`). The only difference is the partition
  comes from the carve instead of a fixed subtype.
- **Remove**: mark free in the store; re-materialize the table without the
  entry. No data moves (v1).

### 4.6 Migration

- Device with a legacy v1.x table: the launcher detects the fixed 3-slot
  table, seeds the carve with the *identical* offsets/sizes after a normal
  scan, commits the store, materializes (a no-op byte-wise), and continues.
  Installed plays are untouched.
- Release gate: `verify_firmware.py` preserved regions change from
  nvs/ota_0/ota_1/ota_2/otadata to nvs/store/otadata + both pool ranges
  (all-0xFF in the artifact so user data survives launcher upgrades).

### 4.7 Failure-mode matrix (power loss)

| Interruption | Result |
|---|---|
| mid image write | slot INVALID — today's semantics, unchanged |
| mid store commit | A/B seq → older valid record wins |
| mid table write (single 4 KB sector) | next boot: hook restores table from store copy (or safe table); otadata wiped → factory |
| store partition dead | hook writes compiled-in safe table → factory; pool content intact but unaddressable → plays need reinstall (accepted, L7) |
| child firmware scribbles the pool | bounded to the pool; hook restores any table touching cardid/factory/otadata; child cannot self-boot (cold-boot otadata wipe) |

## 5. Design boundaries (hard)

- B1: `cardid` @ 0x356000, `factory` @ 0x10000/0x170000, `otadata` @
  0x7FE000 are immutable. Dynslot re-carves only the pool.
- B2: ≤8 slots, 64 KB offset alignment, 4 KB size granularity, 128 KB minimum
  slot. The IDF ceiling is 16 OTA subtypes; we do not approach it.
- B3: total installed capacity ≈ 6.45 MiB minus per-slot tails — no capacity
  miracle; the win is allocation flexibility.
- B4: one reboot per carve-affecting install/remove; play boot under an
  unchanged carve adds none.
- B5: the bootloader hook is the authority; a carved table that does not match
  committed store metadata is repaired before otadata is read.
- B6: the child-firmware contract does not change — a play still sees a
  normal OTA partition (`esp_ota_get_boot_partition`), still returns via OK
  long-press (`metapass_return_to_launcher`).

## 6. Functional limitations

- L1: **No compaction in v1.** First-fit can still refuse an image that would
  fit after defragmentation; the user removes a play instead. Compaction
  (in-flash copy + recarve) is a later phase.
- L2: **ota_2 littlefs dual-use goes away as a fixed property.** Today's
  "recording firmware mounts littlefs on ota_2 when no valid image"
  (meta-pass-design.md §6.3) becomes an optional carve record of type
  `storage`; a device that wants recording reserves one at carve time.
  Default carve: all apps.
- L3: plays that hard-code slot offsets (contract violation) break under
  dynslot.
- L4: phone-side and USB-side installers must adopt the shared allocator
  before the firmware enables dynslot; the manifest version gates mixed
  old-analyzer/new-firmware pairs (device rejects stale geometry proposals).
- L5: per-slot 4 KB tail sector is still consumed per slot (MSIG/MNAM/MAEG
  and the signing story are unchanged).
- L6: the raw Wi-Fi credential backup moves from 0x35A000–0x360000 into a
  dedicated region inside `store`; old backups are lost on migration
  (re-enter Wi-Fi credentials once).
- L7: store-partition death costs the slot map (plays remain in flash but are
  unreachable); recovery is reinstall, not repair.
- L8: downgrade from dynslot to a v1.x launcher is only safe while the carve
  equals the legacy 3-slot offsets; after any resize, downgrading requires
  reflash.

## 7. Out of scope (future work, not this design)

- Compaction/garbage collection and in-place slot resizing.
- More than 8 slots; moving otadata; spanning cardid.
- Pool encryption; secure-boot interplay (irrelevant while both are off).

## 8. Verification plan

- Host pure tests: allocator (first-fit, alignment, min/max), carve↔table
  materialization byte-compared against `gen_esp32part.py` output, MD5 golden
  vectors (incl. the play 563 table bytes), migration seeding from a legacy
  table fixture, failure-matrix transitions.
- Bootloader hook tests (QEMU): corrupt table sector, corrupt store A/B,
  carve/table mismatch, child-scribbled table — each must land on factory.
- E2E on device: install the §1 motivating cases; power-cut injection at each
  phase of the matrix.
- Gates: `tools/validate.sh --static`; updated `tests/test_verify_firmware.py`;
  artifact contract from §4.6.

## 9. Open decisions (need approval before implementation)

1. Max slots = 8, min slot = 128 KB — confirm or adjust.
2. `storage` carve type: keep as an option (L2) or drop recording dual-use
   entirely?
3. Relocating the Wi-Fi credential backup into `store` (L6) — acceptable?
4. otadata stays at 0x7FE000 (recommended; moving it has no upside).
5. Accept one-reboot-per-carve-change (B4)?
