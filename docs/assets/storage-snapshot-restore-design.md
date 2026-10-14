# MetaPass v2 Storage Snapshot Restore Design

Status: implementation on feat/storage; hardware validation pending
Date: 2026-10-15

## 1. Goal and non-goals

A v2 archive is a raw-byte snapshot of the device's currently allocated, restorable Flash partitions, not a logical APP+DATA archive. The USB page retains historical v1.1 archive restore; the embedded phone page accepts v2 snapshots only. Both pages use the shared manifest/ZIP validation and sequential restore coordinator, with transport-specific adapters.

This phase does not implement transaction journals, automatic rollback, staging areas, two-phase commit, DATA resizing, separate DATA deletion, or orchestration-heavy repair logic.

## 2. Archive metadata

The v2 manifest contains the format kind/version, completeness flag, each target partition's label/type/subtype/offset/size, file name/partition-relative offset/length/SHA-256 for every non-erased extent, the source MetaPass version, and the compatible-version range.

Compatibility is conservative by default: the range initially contains only the source version. If version metadata is missing, malformed, or the target version is outside the declared range, restore is rejected before any Flash write and the UI instructs the user to reflash the source MetaPass version and retry. Widen the range only when an explicit compatibility matrix exists.

## 3. Snapshot scope and partition safety

1. Resolve partitions from the current live partition table. Never use absolute Flash addresses supplied by the archive.
2. Identify the running MetaPass host from device runtime information (phone API) or uniquely from the USB-read ESP app descriptor whose project name is FoloToy-AI-Passport. Fail closed if identity is ambiguous.
3. Exclude the current MetaPass host partition, cardid, read-only/protected partitions, and unallocated Flash gaps.
4. Preserve raw bytes from allocated NVS and dynamic storage partitions. Every target is selected by live partition identity and geometry; the restore adapter accepts partition identity plus relative offset.
5. Device endpoints re-resolve and verify label/type/subtype/offset/size on every read, erase, and write, and reject cardid, the running host, read-only partitions, and out-of-bounds requests.

## 4. Restore stages

### Stage A: read-only preflight

1. Parse ZIP and manifest; validate format version, completeness, and extent geometry.
2. Validate the byte length and SHA-256 of every payload; reject missing or undeclared payloads.
3. Read the target's live partition table, MetaPass version, running-host identity, and protected ranges.
4. Validate the version range and exact restorable partition set; compare label/type/subtype/offset/size and reject overlapping partitions.
5. Build the restore plan. Any failure here must result in zero Flash writes.

### Stage B: sequential partition restore

For each planned partition:

1. Erase only that live partition's full range.
2. Write each raw extent at its partition-relative offset.
3. Independently read back the entire partition in 4096-byte chunks and compare every byte. Sparse areas omitted from extents must read as 0xFF.
4. Stop immediately on any failure. Do not continue to later partitions and do not claim rollback. Report phase, partition label, relative offset, expected/actual bytes when available, and the error.

Error categories:

- SNAPSHOT_RESTORE_WRITE_FAILED: partition erase or write failed.
- SNAPSHOT_RESTORE_READBACK_FAILED: independent readback failed.
- SNAPSHOT_RESTORE_READBACK_MISMATCH: actual bytes differ from the snapshot.
- SNAPSHOT_RESTORE_BOOT_VALIDATION_FAILED: data readback passed, but later reboot/reconnect validation failed.

For the first three categories, the UI marks restore incomplete/inconsistent and recommends reflashing the source MetaPass version recorded in the manifest, reconnecting, and retrying. Do not claim an automatic data rollback or that the device remains unchanged after failure.

### Stage C: boot validation

Successful byte comparison proves the stored data matches the snapshot; it does not prove the device booted. Both USB and phone pages currently ask the user to leave download mode/reboot and confirm MetaPass reconnects. If a reliable reboot/reconnect API is added later, keep it as a separate phase from data integrity.

## 5. UI principles

- Before any erase, show the source MetaPass version, number of partitions to overwrite, and the recovery path: reflash that version and retry if the restore is interrupted or verification fails.
- Do not describe unimplemented transaction safety or automatic rollback.
- Preflight failure: state that no Flash was written.
- Write/readback failure: report restore incomplete/inconsistent, partition, relative offset, and source version.
- If data verification passes but boot was not checked, ask the user to reboot/reconnect; do not show boot validation as PASS.
- ZIP files are unencrypted and may contain Wi-Fi credentials, device state, and child-firmware data.

## 6. Automated and hardware acceptance

Host tests cover source-version metadata and fail-closed version checks, zero writes on incompatible versions, layout/protected-partition mismatch, partition erase and extent writes, full readback including sparse 0xFF regions, exact mismatch location, stopping after write failure without rollback, and separation of data verification from boot validation.

Hardware validation is still required for both USB and phone transports, actual erase/write/readback behavior, and post-restore reboot/reconnect. Host mocks are not hardware evidence.