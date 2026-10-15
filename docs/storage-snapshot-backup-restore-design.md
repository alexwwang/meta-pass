<p align="right">
  <strong>English</strong> · <a href="storage-snapshot-backup-restore-design.zh_CN.md">简体中文</a>
</p>

# Storage Snapshot Backup and Restore Design (2026-10-15)

## Decision

For v2, backup and restore use a **device-storage snapshot**, not a logical archive grouped by APP and DATA ownership. The snapshot preserves bytes in allocated storage partitions without interpreting child-firmware formats, NVS keys/namespaces, or filesystem internals.

This supersedes the earlier proposal to archive one APP+DATA group at a time and to preserve the target device's NVS unchanged during restore. NVS is raw snapshot content like every other eligible partition; it has no special archive, validation, or write path.

## Snapshot scope

Use the connected device's live partition table to determine allocated partitions eligible for inclusion. Exclude:

1. The partition containing the currently running MetaPass host firmware.
2. The `cardid` partition, which protects device identity and factory recovery.
3. Unallocated Flash gaps and all regions outside eligible partitions.

Do not hard-code Flash addresses. Identify the host firmware partition from the device's current running application/OTA partition report. The partition table describes the layout; it is not snapshot payload and must never be written back.

NVS is not special in this model: include it if it is an allocated partition and is not otherwise excluded. Restoring a snapshot may therefore restore the host NVS—including Wi-Fi credentials and installation-session keys—to the state captured at backup time. The UI must warn that archives may contain sensitive information and that restore replaces included target storage. Do not promise that target NVS remains unchanged.

## Byte representation and sparse storage

- Record each included partition's stable identity (label, type, subtype), source offset, size, and payload extents in the manifest.
- Erased `0xFF` runs may be sparse-encoded, but their positions and lengths must remain recoverable so restore reproduces the original byte layout.
- Do not semantically filter bytes within an included partition. An all-`0xFF` range inside an included partition is sparse data, not an unallocated address gap.
- Verify every payload extent's length and SHA-256 before any write.
- A snapshot must be complete. A read failure must never silently produce a restorable partial archive.

## Restore rules

1. Parse and validate the complete archive. Reject duplicate or unsafe paths and verify every payload hash and range before starting writes.
2. Read the target device's live partition table and identify its currently running host firmware partition.
3. Require exact layout compatibility for every included partition. The initial implementation requires identity, offset, and size to match; reject mismatches rather than guessing or silently remapping.
4. Recompute protected ranges on the target. Reject archive entries that cover the running MetaPass firmware, `cardid`, the partition table, or unallocated Flash.
5. Erase/write only eligible ranges. Restore omitted sparse erased ranges to erased state as needed, then read back and verify. Every eligible partition, including NVS, follows the same partition-identity plus relative-offset byte I/O path.
6. Never write the running MetaPass firmware, `cardid`, partition table, or unallocated gaps. Do not separately rewrite allocator/carve metadata unless it resides in an explicitly included partition.

## Compatibility and page parity

- USB retains historical v1.1 fixed-slot archive restore. The embedded phone page accepts v2 snapshots only.
- Both pages share the v2 manifest, extent validation, snapshot capture, and sequential restore coordinator. USB alone retains local-firmware installation.
- Old APP+DATA logical v2 archives must not be mistaken for snapshots; reject them with a clear error.
- The manifest records the source MetaPass version and an inclusive compatible-version range. Until a broader compatibility matrix is maintained, the range is conservatively set to the exact source version. A target-version mismatch must be resolved by reflashing the source version before retrying.

## Implementation status and next steps

1. **Implemented (shared snapshot foundation):** manifest/extent validation, ZIP encode/decode, payload length/SHA-256 validation, sparse erased-byte representation, target-layout preflight, and a shared capture function used by both transports.
2. **Implemented (device API):** authenticated storage inventory reports the running MetaPass version and host identity; bounded partition-relative read, erase, and write endpoints re-resolve the live partition and reject the running host, cardid, read-only partitions, and out-of-bounds ranges.
3. **Implemented (USB transport):** identifies the MetaPass host from a validated ESP app descriptor and fails closed if it cannot uniquely identify it. It retains the historical v1.1 archive path and supports v2 raw snapshot backup/restore on dynamic layouts.
4. **Implemented (phone transport):** v2 snapshot backup and sequential restore use the shared capture/restore core. Phone v1 archive compatibility is intentionally not added.
5. **Implemented (restore behavior):** full preflight before any write; per-partition erase, extent writes, and independent full-partition readback, including sparse 0xFF regions. Any failure stops the sequence and reports partition/relative offset. No transaction journal, automatic rollback, or complex repair orchestration is implemented.
6. **Still required:** real-device verification of both transports, including byte-level persistence after restore and a separate reboot/reconnect check. Host mocks and CI are not hardware evidence.

## Recovery and UI rules

- Before writing, show the source MetaPass version and number of partitions to overwrite.
- On preflight failure, state that no Flash was written.
- On erase/write/readback failure, state that restore is incomplete/inconsistent, include the partition and relative offset, and recommend reflashing the source MetaPass version before retrying.
- Data readback success is not boot validation. Both pages currently require the user to exit download mode/reboot and confirm that MetaPass reconnects.
- Archives are unencrypted and may contain Wi-Fi credentials, device state, and child-firmware data.

## Explicitly out of scope

- Do not parse or merge NVS keys/namespaces.
- Do not infer ownership of individual keys in shared NVS.
- No cross-layout address remapping in the initial version.
- No resizing of existing DATA partitions as part of backup/restore.
