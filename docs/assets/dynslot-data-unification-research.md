<p align="right">
  <a href="dynslot-data-unification-research.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Child-Firmware Data Storage: Takeover/Mapping Feasibility

Date: 2026-10-02
Branch: `feat/dynslot`
Companion: `dynslot-design.md` (pool + carve). This document answers one
question: **can meta-pass take over / unify where child firmware (plays) keep
their data, so storage can be shared and managed centrally?**

## 1. Verdict (conclusion first)

**Partially feasible — three tiers, no runtime interception.**

| Tier | Access pattern | Feasible? | Mechanism |
|---|---|---|---|
| T1 | Data addressed by **partition label** (NVS partition label, SPIFFS/littlefs mount by label, `esp_partition_find_first(label)`) | **Yes, zero child change** | Carve-time label aliasing: the carved table declares the child's own declared data partitions, same labels, pool offsets (§4, M1) |
| T2 | Default shared `nvs` partition (namespace-based keys) | **Manageable, not redirectable** | Namespace governance: convention + enumeration/eviction via `nvs_entry_find(part, NULL, …)` (§4, M2) |
| T3 | **Raw fixed-offset** `spi_flash_*` / `esp_flash_*` access | **No** | Nothing between a running child and the flash. Contract-level mitigation only (§4, M4 rejected) |

Runtime interception of a running child's storage calls (VFS hooking,
`spi_flash` driver patching, cache-MMU remapping) is **rejected**: a child
links its own ESP-IDF copy, registers its own VFS, and owns its MMU mappings.
There is no supported interception point, and dynslot already gives meta-pass
the only layer that actually governs layout — the partition table.

## 2. What plays actually do (measured)

### 2.1 Ecosystem sample: six play binaries decoded at byte level

| Play | factory size | declared data partitions | child-installable today? |
|---|---|---|---|
| Event Dashboard (829) | **8,128K** | none | no — exceeds every slot |
| Fishing Guide (733) | **8,128K** | none | no |
| Butt Detective (826) | **8,128K** | none | no |
| RELAY Walkie-Talkie (842) | **8,128K** | none (nvs shrunk to 16K) | no |
| Pixel Badge (523) | 7,936K | `content` data 192K @ 0x7D0000 | no |
| AppStore (563) | 3,072K | `store` NVS 16K + `easter` 388K | yes |

Facts this establishes:

1. **Most plays are whole-flash standalone images** (factory ≈ 8 MB, no OTA
   slots, no cardid). They are flashed at 0x0 and own the device; they are
   not child firmware in the meta-pass sense and never will be without a
   rebuild. Only template-aware plays built against a slot layout (play 563
   is the reference) are installable children.
2. **Among installable-style plays, declared data partitions are rare** —
   play 563's `store`/`easter` is the only sampled case; Pixel Badge carries a
   192K `content` data partition. Data management is a real but
   forward-looking problem.
3. **Data partition *contents* are silently dropped by meta-pass install
   today**: `install-slot/extract-app-image.js` extracts the factory app
   image only; a child's data partitions are parsed never and installed
   never. Any data unification design also fixes this lossy gap.
4. The reference Voice-Keychain play mounts SPIFFS `voicefs` at `/voices`
   via `esp_vfs_spiffs_register` by partition label
   (`official-main/docs/reference/shinku-chen/voice-keychain/voice-guide.md:38-42`)
   — the canonical label-addressed pattern (T1).

### 2.2 Upstream template facts

- Stock table (`official-main/partitions.csv:1-5`): nvs 24K, phy_init,
  factory 3M, cardid. No storage/data partition.
- **Zero NVS application usage in upstream code** (no `nvs_open` anywhere in
  official-main). No namespace convention exists; each play invents its own.
- Stock recording is RAM-only (`official-main/main/demo_audio.c:60-89`);
  the flash-persistent recording convention is prose, not code
  (`official-main/docs/README.md:44-45`).

### 2.3 meta-pass's own storage (the launcher is also a "child" of its table)

- Fully partition-API based: `esp_partition_find_first` by subtype
  (`main/meta_store.c:19-25`) — slot images, tail sector (MSIG/MAEG/MNAM,
  image-relative), otadata. All redirectable for free under carve.
- **One raw-offset offender**: Wi-Fi credential backup at hard-coded
  `0x35A000` (MPCK magic + crc32, `main/meta_store_net.c:119-178`) — T3,
  relocates into the `store` region under dynslot (L6 in dynslot-design.md).
- NVS namespace `metapass` for Wi-Fi creds; documented fragility: child
  firmware boot can format the shared nvs partition
  (`main/meta_store_net.c:119-124`).
- Child-facing contract (`main/metapass_hook.h`) exposes **no storage APIs
  at all** — only signature self-diagnostic and return-to-launcher.

### 2.4 Recording & retro-console deep-dive (measured)

These are exactly the categories the question asks about — firmware whose
function depends on multi-megabyte flash data archives. Five more binaries
decoded at byte level:

| Play | App | Declared data partitions | Access pattern (from binary strings) |
|---|---|---|---|
| Voice Recorder (28) | factory 1,536K | `recordings` **6,592K** (0x81 = FAT, all-0xFF at ship) | FatFS: `esp_vfs_fat_spiflash_mount_rw_wl`, mount error string names `partition_label='%s'` — **mounted by label** (T1). Web UI exports recordings as WAV, supports delete — pure user data, runtime-generated |
| GameBoy Handheld (494) | game_app 3,072K | `roms` 4,096K (0x40, magic `FGBR` = bundled ROM pack) + `saves` 960K (0x82 = SPIFFS, mounted at `/saves`) | SPIFFS (`esp_spiffs.c`) + POSIX `/saves` paths + NVS blobs — **label-addressed** (T1). Two data classes: bundled content shipped in-image + runtime save archive |
| Mini Console (115) | factory 1,536K | `storage` 5,568K (0x82, magic `NESPACK1` = bundled NES ROM pack) | label `storage` present in strings; NVS for BLE — T1 |
| Pocket Arcade (204) | factory 3,072K | none (template-aware: cardid + recovery) | child-compatible as-is |
| Handheld Arcade (793) | factory **8,128K** | none | whole-flash standalone, not a child |

Facts this establishes:

1. **Archive-carrying plays are label-addressed (T1) without exception** —
   FatFS/SPIFFS mounts by partition label through standard IDF APIs. M1
   aliasing covers them with zero child changes.
2. **The IDF filesystem mounts are size-dynamic**: `esp_vfs_fat_*` /
   `esp_vfs_spiffs_*` derive the backing size from the partition table entry.
   A carve may therefore **downsize** a declared data partition (e.g.
   `recordings` 6,592K → 4M) and the firmware keeps working — no author
   rebuild — as long as the firmware never hard-codes the size. (Correctness
   of a shrunken FAT/SPIFFS *image* only matters when in-image content ships;
   an empty-at-ship archive formats cleanly at any size.)
3. **Capacity is the binding constraint** (pool 6,766,592 B):
   - Voice Recorder: app + recordings = 1,572,864 + 6,753,280 ≈ 8.1 MB → full-size
     child impossible; **downsized recordings carve works** (pool − app −
     tails ≈ 4.9 MB available). Demonstrable v1 case.
   - GameBoy: 3M + 4M + 960K ≈ 7.9 MB → impossible without the author
     trimming the bundled `roms` pack; `saves` then rides M5 below.
   - Mini Console: 1.5M + 5.5M ≈ 7.1 MB → author trim needed.
4. **Two archive classes need different lifecycles**: bundled content
   (shipped bytes, disposable — reinstall restores it) vs. runtime archives
   (recordings, saves — must survive play upgrades and ideally uninstall).
   This split drives M5.

## 3. Why runtime interception is off the table

| Candidate | Why it fails |
|---|---|
| VFS hooking | VFS is a per-registration driver table; the child registers its own mounts with its own IDs. No global pre-emption point. |
| `spi_flash`/`esp_flash` patching | Child links its own IDF; symbols resolve inside the child. No supported override. |
| Cache-MMU remapping | Mappings are established by the IDF startup for the app's own segments; remapping flash regions per-child is not a supported configuration and fights the bootloader's own mapping. |
| Flash encryption as a gate | Would change the trust model wholesale; both secure boot and flash encryption are off and are their own project. |

And per AGENTS.md:20, whatever *is* enforced must live at the
bootloader/validator layer — which under dynslot is exactly the carve table +
`meta_boot_hooks` validation. That layer governs **layout**, not runtime
calls; the design below stays at that layer.

## 4. Feasible mechanisms

### M1 — Carve-time label aliasing (T1, primary)

The child's merged image already declares its full partition table; phone-side
analyze (`extract-app-image.js`) already parses it. Extend analyze to emit the
child's **data partition requirements** `{label, type, subtype, size}`.
The carve then:

1. reserves pool space for each declared data partition,
2. materializes table entries with the **same labels/types** at pool offsets
   (offsets are per-carve, so collisions across slots are impossible),
3. installs the data partition *contents* from the merged image alongside the
   app image (fixes the §2.1-3 lossy gap),
4. records all of it in the store metadata as per-slot **data carve records**
   `{label, type, offset, size, sha256}`.

Child firmware needs **no changes**: it calls
`esp_partition_find_first(DATA, x, "store")` / `esp_vfs_littlefs_register`
with its own label and gets a real partition. Verified IDF behavior making
this sound: partition lookup iterates the loaded table in order and
`esp_partition_find_first` returns the first match
(`esp_partition/esp-idf-v5.5.3/components/esp_partition/partition.c:359-381`);
carve entries use each child's own labels, so no aliasing ambiguity exists —
the only label that cannot be duplicated per-slot is the default `nvs`
(T2).

Management wins once data is carved: per-play erase on uninstall (today:
orphaned data), per-play quota in the store metadata, backup/restore of a
play's data with its image, and migration under compaction.

### M2 — Shared `nvs` governance (T2)

The default 24K `nvs` partition cannot be per-slot redirected (single label,
first-match lookup). Manageable instead:

- **Convention**: children use namespace `play:<slug>`; meta-pass reserves
  `metapass` (system) and `play:*` prefixes.
- **Enforcement by enumeration**: `nvs_entry_find(part_name, NULL,
  NVS_TYPE_ANY, &it)` iterates every entry in every namespace
  (IDF 5.5.3 `nvs_flash/include/nvs.h:716`); non-whitelisted namespaces can
  be listed and deleted (`nvs_erase_key`/namespace cleanup) from the launcher
  at boot and on uninstall. Blast radius of a rogue child is capped at 24K.
- **Known hazard stays partially open**: a child calling `nvs_flash_init()`
  and *formatting* on any error wipes system credentials too
  (`meta_store_net.c:119-124`). The MPCK raw backup already covers this;
  dynslot's relocation of that backup into `store` keeps the safety net.

### M3 — Optional child SDK data API (T2/T1 convenience, cooperation-based)

Extend `metapass_hook.h` with an opt-in data layer (mount helper returning
the play's own carve by label convention, KV API backed by the play's NVS
carve). Better ergonomics and a place to hang quotas/sharing, but purely
cooperative — adapted children only. Not required for M1/M2 to work.

### M4 — Shared partitions, generalized (size = device config item)

Generalized dedupe rule: carve partitions are unique per `(label, type)`;
plays declaring an identical `(label, type)` pair map to the **same**
partition (e.g., two recording apps both declaring `recordings`/fat share one
archive). The old fixed `share` partition becomes just a convention label
under this rule.

**Size is a device configuration item**, decided per the user, not hard-coded:
each shared partition's size is a fixed carve record in the store metadata,
adjustable from the launcher UI / phone page (a carve change, so one reboot).
Default: no shared partitions (0 B); range clamped (e.g. 256K–2M); runtime
archives like recordings are the intended use. Bundled-content partitions are
excluded from sharing — their `(label, type)` matches only when the in-image
bytes match (sha256 in the analyze output), else they get private carves.

### M5 — Detached data-carve lifecycle (runtime archives)

Data carve records are **keyed by play identity (slug), not by slot index**,
so they outlive the app image they belong to:

```
data_record = { slug, label, type, subtype, offset, size,
                declared_sha256, state: PRISTINE | DIRTY | ARCHIVED }
```

- **First install**: allocate per analyze; install in-image content (state
  PRISTINE; `declared_sha256` pins the pristine bytes).
- **Reinstall / upgrade of the same slug**: records with matching labels are
  **kept as-is** — saves and recordings survive the upgrade. Labels the new
  version dropped become ARCHIVED; new labels allocate fresh.
- **Runtime writes**: the play flips its records DIRTY (soft signal via the
  launcher-visible tail sector or simply implied by play boot); DIRTY content
  is not sha256-verifiable, so backup/restore relies on FS-level integrity
  (FatFS WL / SPIFFS gc). PRISTINE records can always be restored from the
  install image and need no backup.
- **Resize** (new version declares a different size): v1 = allocate a new
  record, copy in-pool, ARCHIVE the old (reclaimed under pool pressure).
  In-place growth lands with the compaction phase.
- **Uninstall**: default ARCHIVE (user data kept, reclaimable); explicit
  "erase data" choice erases. Bundled-content records default to erase.

## 5. Unified model (schema delta on dynslot store metadata)

```
slot[i] += data_count, data[j] = { label, type, subtype, offset, size, sha256 }
carve  += shared_partitions[k] = { label, type, size }   // device config items
```

Detached runtime archives (M5) live outside `slot[i]` in a top-level
`data_records[]` keyed by slug, so they survive slot recarving.

Lifecycle hooks in the launcher: install (M1 alias + content restore),
uninstall (M5 archive/erase policy), boot (NVS whitelist sweep under M2),
migration (dynslot compaction moves data records with their slots).

## 6. Boundaries and limitations

- D1: T3 (raw-offset children) cannot be mapped, detected reliably, or
  contained beyond the pool. The ecosystem contract must keep discouraging
  raw offsets; meta-pass itself relocates its only offender (MPCK).
- D2: Whole-flash standalone plays (~8 MB factory, the majority of the
  catalog) are out of scope for child data management entirely — they are
  not children. Archive-heavy plays (GameBoy, Mini Console) exceed the pool
  even as children and need author-trimmed builds; M1's size-dynamic carve
  can absorb moderate downsizes without rebuilds, bundled-content shrinks
  cannot.
- D3: Default-`nvs` children without namespace discipline share 24K with
  system credentials; M2 caps but does not eliminate the risk. Eradication
  requires children to adopt M1-style own partitions or M3.
- D4: A running child has full flash access; carve governance is layout
  authority, not a sandbox. Trust level unchanged from today.
- D5: Data carve consumes pool space; a child declaring large data
  partitions shrinks what is left for apps. Quota and analyze-time
  rejection apply.
- D6: The unified model is only as strong as analyze's parsing of the
  child's embedded table — malformed child tables are rejected at install
  (existing format gate), not reinterpreted.
- D7: Shared partitions (M4) couple plays that share them: erasing the
  shared `recordings` archive to satisfy one play's uninstall affects the
  others — hence ARCHIVE-by-default and user-visible config.
- D8: Downsize-by-carve (§2.4-2) assumes the firmware derives FS size from
  the partition table, which standard IDF mounts do but a firmware with its
  own size constant would not; analyze cannot detect that statically — first
  hardware run per such play is the verification.

## 7. Recommendation

1. Fold **data carve records** (M1/M5 schema) into the dynslot store metadata
   now — reserved fields, zero cost.
2. Ship **M1** with dynslot v1 — same carve mechanism, more entry types;
   repairs the dropped-data-partition gap; demonstrable with Voice Recorder
   (recordings carve downsized to fit the pool).
3. Ship **M2** (namespace whitelist sweep) with dynslot v1 — cheap, caps the
   known nvs hazard.
4. Ship **M5** lifecycle (survive-upgrade saves/recordings) with dynslot v1 —
   it is metadata policy plus allocator rules, no new mechanisms.
5. **M4** shared partitions as a device config item lands with the carve-UI
   phase; **M3** as follow-up; **runtime interception: never**.
