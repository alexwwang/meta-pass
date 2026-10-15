<p align="right">
  <a href="mobile-page-data-e2e-market-comparison.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Marketplace Voice and E-book Firmware: DATA Storage Comparison

Date: 2026-10-09
Branch: `feat/storage`

## Executive summary

- **DeepSeek voice / FoloToy AI Passport is not equivalent to Recorder Play 28.** Public XiaoZhi AI Passport source targets ESP32-C3 with 8 MiB flash. Its v2 partition table has shared NVS and a 2 MiB `assets` partition with SPIFFS subtype, but no dedicated recording/user-archive DATA partition. Settings use NVS; `assets` is found by label and read/memory-mapped. Voice interaction primarily streams audio. This is useful for NVS/assets analysis, not the first choice for per-play FAT user-data lifecycle acceptance.
- **An open-source e-reader reference does have a dedicated user-data partition, but it is not confirmed to be the specific marketplace e-book play.** CHENXiNNNX/Ebook uses 6 MiB FAT `userdata`, 4 MiB LittleFS `assets`, and an external TF card. It is a source reference, not a substitute for the actual marketplace binary.
- **Recorder Play 28 remains the best current marketplace DATA acceptance target.** Existing marketplace-binary analysis confirms a `recordings` FAT partition, runtime-generated recordings, WAV export, and deletion. Shrinking and real-device UI automation remain incomplete.

## Evidence levels

| Target | Evidence available | Confirmed | Not yet established |
|---|---|---|---|
| FoloToy AI Passport / DeepSeek configuration | Public source, board config, v2.5.0 release metadata | Partition table, NVS settings API, assets lookup/access | Release ZIP has not been locally inspected byte-by-byte; exact identity with any marketplace listing is not established |
| Open-source Ebook project | Public source, partition CSV, user-data layout | Separation of FAT userdata / LittleFS assets / TF card | Not confirmed to be the target marketplace e-book binary |
| Recorder Play 28 | Existing byte-level marketplace firmware research in this repository | `recordings` FAT, label-based mount, runtime recordings can be exported/deleted | Exact marketplace version and automated UI contract remain unpinned |

## 1. DeepSeek voice: FoloToy AI Passport / XiaoZhi AI

Public upstream: `78/xiaozhi-esp32`, release `v2.5.0`, which lists `v2.5.0_folotoy-ai-passport.zip` (about 2.23 MB). `main/boards/folotoy/ai-passport/config.json` sets target `esp32c3`, 8 MiB flash, and `partitions/v2/8m.csv`.

- Upstream: https://github.com/78/xiaozhi-esp32
- Board config: https://github.com/78/xiaozhi-esp32/blob/main/main/boards/folotoy/ai-passport/config.json
- Partition table: https://github.com/78/xiaozhi-esp32/blob/main/partitions/v2/8m.csv
- Release: https://github.com/78/xiaozhi-esp32/releases/tag/v2.5.0

Here “DeepSeek voice” means a voice firmware whose model provider can be configured as DeepSeek, not a distinct DeepSeek-specific filesystem implementation.

### Partition layout (v2 / 8 MiB)

| Label | Type / subtype | Size | Observed role |
|---|---|---:|---|
| `nvs` | data / nvs | 16 KiB | Persistent settings |
| `otadata` | data / ota | 8 KiB | OTA boot state |
| `phy_init` | data / phy | 4 KiB | PHY initialization |
| `ota_0` | app / ota_0 | 2.94 MiB | Application image |
| `ota_1` | app / ota_1 | 2.94 MiB | Application image |
| `assets` | data / spiffs | 2 MiB | Packed resources, found by label and read/mapped by source |

There is no dedicated `recordings`, `userdata`, or other FAT archive partition.

### Source-level storage behavior

- `main/settings.cc` uses `nvs_open`, `nvs_get_*`, `nvs_set_*`, and `nvs_commit` for settings. NVS key/value persistence is not a user-visible recording directory; in meta-pass's current model, default `nvs` is not an isolated per-play label carve.
- `main/assets.cc` finds `assets` with `esp_partition_find_first(..., "assets")`, reads a header using `esp_partition_read`, and accesses packed resources through `esp_partition_mmap`. This is not a standard FAT recording directory.
- Voice interaction primarily uses audio capture, network transport, and playback. “Accepts voice input” does not imply that raw microphone audio is persisted as local files. The reviewed public source did not reveal a dedicated DATA partition for per-turn recording files.

### Relevance to meta-pass

Useful for studying NVS persistence and labeled `assets` access. Not the best first target for DATA write/read/hash/reboot/delete acceptance because no equivalent dedicated FAT user-data lifecycle was found.

## 2. E-book: open-source reference implementation

The search found CHENXiNNNX/Ebook, an open-source ESP32-S3 e-reader. It is a source reference, **not a confirmed match for the marketplace e-book play**.

- Source: https://github.com/CHENXiNNNX/Ebook
- Partition table: https://github.com/CHENXiNNNX/Ebook/blob/main/partitions/partition_16mb.csv
- User-data layout: https://github.com/CHENXiNNNX/Ebook/blob/main/assets/fat_userdata/README.txt

| Label / medium | Format / size | Access | Typical content |
|---|---|---|---|
| `userdata` | FAT, 6 MiB | `esp_vfs_fat_spiflash_mount_rw_wl("/int", "userdata", ...)` | TXT books, notes, exported drawings |
| `assets` | LittleFS, 4 MiB | `esp_vfs_littlefs_register`, read-only `/assets` | Fonts and UI assets |
| TF card | External FAT32 | `esp_vfs_fat_sdmmc_mount`, `/sd` | User-imported books/files |
| `nvs` | NVS, 16 KiB | NVS API | Device settings |

The source mounts FAT+wear levelling by the `userdata` label, mounts LittleFS read-only by the `assets` label, and scans fixed paths such as `/int/Ebook/txt` and `/sd/Ebook/txt`.

This pattern is similar to Play 28 at the label-addressed filesystem layer (T1), but has different lifecycle and capacity constraints: the 6 MiB userdata plus app is not a direct fit for the current dynamic pool; content may live on internal flash or TF card; factory-seeded files must be distinguished from runtime user data. The actual marketplace e-book binary/source is still needed to establish its behavior.

## 3. Suitability comparison

| Dimension | Recorder Play 28 | AI Passport / DeepSeek voice | Open-source Ebook reference |
|---|---|---|---|
| Dedicated DATA label partition | Yes: FAT `recordings` | Not found; shared NVS + `assets` | Yes: FAT `userdata` |
| User files directly observable | WAV export/delete | No local voice recording flow found | Book/note files |
| T1 label lookup | Yes | Yes for assets, but not user FAT archive | Yes |
| Dynamic pool fit | Candidate after shrinking to ~4 MiB; unverified | Needs actual image/partition admission review | 6 MiB userdata + app is not a direct fit |
| Best first real DATA E2E | **Yes** | No; better for NVS/assets-specific coverage | Useful source reference, not target market binary |

## 4. Download and verification status

The public release asset and source were located, and the source/partition table were reviewed. The current execution environment could not download the release ZIP locally (network DNS/binary download channel unavailable), so this document does **not** claim a local binary unpack, SHA-256 record, or byte-level analysis of that ZIP. In a network-enabled environment, download `v2.5.0_folotoy-ai-passport.zip`, record SHA-256, unpack its partition table, app image and assets, then compare against the exact marketplace listing binary.

## Decision

Keep Play 28 as the real marketplace DATA lifecycle target; use AI Passport / DeepSeek voice as an NVS + assets comparison case; use open-source Ebook as a reference for FAT user data, read-only assets, and external SD storage. The exact marketplace e-book play remains unverified and must not be replaced by the open-source reference.
