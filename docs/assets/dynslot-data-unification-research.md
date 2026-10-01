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
| 赛事看板 (829) | **8,128K** | none | no — exceeds every slot |
| 钓鱼图鉴 (733) | **8,128K** | none | no |
| 屁屁侦探 (826) | **8,128K** | none | no |
| RELAY 通话器 (842) | **8,128K** | none (nvs shrunk to 16K) | no |
| 像素工牌 (523) | 7,936K | `content` data 192K @ 0x7D0000 | no |
| AppStore (563) | 3,072K | `store` NVS 16K + `easter` 388K | yes |

Facts this establishes:

1. **Most plays are whole-flash standalone images** (factory ≈ 8 MB, no OTA
   slots, no cardid). They are flashed at 0x0 and own the device; they are
   not child firmware in the meta-pass sense and never will be without a
   rebuild. Only template-aware plays built against a slot layout (play 563
   is the reference) are installable children.
2. **Among installable-style plays, declared data partitions are rare** —
   play 563's `store`/`easter` is the only sampled case; 像素工牌 carries a
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

### M4 — Shared zone for cross-play data

One label-fixed `share` data partition (e.g., 256K–1M, carved once, outside
any slot) for genuinely cross-play data (recordings, common assets).
Convention + documentation; children address it by the fixed label. Size is
a carve-time decision, so it costs pool space — make it optional and small by
default.

## 5. Unified model (schema delta on dynslot store metadata)

```
slot[i] += data_count, data[j] = { label, type, subtype, offset, size, sha256 }
carve  += optional fixed records: { label:"share", ... }
```

Lifecycle hooks in the launcher: install (M1 alias + content restore),
uninstall (erase the slot's data carve records; archive optionally), boot
(NVS whitelist sweep under M2), migration (dynslot compaction moves data
records with their slots).

## 6. Boundaries and limitations

- D1: T3 (raw-offset children) cannot be mapped, detected reliably, or
  contained beyond the pool. The ecosystem contract must keep discouraging
  raw offsets; meta-pass itself relocates its only offender (MPCK).
- D2: Whole-flash standalone plays (~8 MB factory, the majority of the
  catalog) are out of scope for child data management entirely — they are
  not children.
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

## 7. Recommendation

1. Fold **data carve records** into the dynslot store metadata schema now
   (reserved fields, zero cost).
2. Ship **M1** with dynslot v1 — it is the same carve mechanism, just more
   entry types; it also repairs the dropped-data-partition gap.
3. Ship **M2** (namespace whitelist sweep) with dynslot v1 — cheap, caps the
   known nvs hazard.
4. **M3/M4** as follow-ups; **runtime interception: never**.
