 <p align="right">
  <strong>English</strong> · <a href="storage-snapshot-backup-restore-design.zh_CN.md">简体中文</a>
</p>

# Storage Snapshot Backup and Restore Design (2026-10-14)

## Decision

For v2, backup and restore use a **device-storage snapshot**, not a logical archive grouped by APP and DATA ownership. The snapshot preserves bytes in allocated storage partitions without interpreting child-firmware formats, NVS keys/namespaces, or filesystem internals.

This supersedes the earlier proposal to archive one APP+DATA group at a time and to preserve the target device's NVS unchanged during restore.

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
5. Erase/write only eligible ranges. Restore omitted sparse erased ranges to erased state as needed, then read back and verify.
6. Never write the running MetaPass firmware, `cardid`, partition table, or unallocated gaps. Do not separately rewrite allocator/carve metadata unless it resides in an explicitly included partition.

## Compatibility and page parity

- The USB page must retain its historical v1.1 archive restore path unchanged.
- USB v2 creates and restores the new snapshot format.
- The embedded phone page supports v2 only and must share the v2 manifest validator, protected-range policy, sparse-extent semantics, and restore checks with USB.
- Local firmware installation remains USB-only. Other storage backup/restore semantics must match across both pages.
- Old APP+DATA logical archives must not be mistaken for snapshots. Use an unambiguous archive kind/version discriminator and show a clear error for ambiguous archives.

## Implementation order

1. Implement the shared pure manifest/extent validator and tests for overlapping ranges, protected partitions, unallocated gaps, sparse erased ranges, hash/length failures, incompatible layouts, and incomplete archives.
2. Implement USB snapshot backup/restore using bounded chunked Flash reads/writes while preserving the v1.1 branch.
3. Add device-side partition inventory/read/write APIs for the phone page. The device must resolve labels and validate absolute ranges against its live partition table and protected regions; never expose unrestricted arbitrary-address writes.
4. Connect the phone v2 page to the shared snapshot format/validator and guarded device-side partition APIs.
5. Update UI copy, design docs, and handoff. Run USB/phone browser automation first, then have the local agent run passport-sim and real-device checks.

## Explicitly out of scope

- Do not parse or merge NVS keys/namespaces.
- Do not infer ownership of individual keys in shared NVS.
- No cross-layout address remapping in the initial version.
- No resizing of existing DATA partitions as part of backup/restore.
