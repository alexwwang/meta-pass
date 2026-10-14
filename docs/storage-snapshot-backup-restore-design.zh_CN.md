# Storage snapshot backup/restore design (2026-10-14)

## Decision

The v2 backup/restore contract is a **device-storage snapshot**, not an APP+DATA ownership archive. The goal is to preserve the current device's persisted bytes without interpreting sub-firmware formats, NVS keys, namespaces, or filesystem internals.

This supersedes the previous v2 proposal to back up one APP+DATA group and to preserve destination NVS during restore.

## Snapshot scope

Build the allowed range list from the connected device's live partition table. Include allocated storage partitions and their meaningful byte contents, with these exclusions:

1. The currently running MetaPass host firmware partition.
2. The `cardid` partition (device identity/factory-recovery boundary).
3. Unallocated Flash gaps and other regions that are not owned by an eligible partition.

Do not hardcode Flash addresses. Resolve partitions from the live partition table and identify the running host app from device-reported OTA/app partition information. Exclude the partition table itself from writes; it is used as the layout descriptor.

NVS is not a special case in this model: if it is an allocated partition other than an explicit exclusion above, its bytes are part of the snapshot. Consequently, restoring a snapshot can restore host NVS state (including Wi-Fi credentials and install-session keys) to the captured state. The UI must warn users that archives may contain secrets and that restore replaces eligible destination storage bytes. There is no promise that destination NVS remains unchanged.

## Byte representation and sparse encoding

- Each included partition has a manifest record with stable identity (label, type, subtype), source offset, partition size, and a list of stored extents.
- Preserve exact offsets and lengths for every non-erased extent. Erased `0xFF` runs may be omitted from the ZIP only when their location and length are recorded so restore can recreate them.
- Bytes inside an included partition are not semantically filtered. An all-`0xFF` extent inside a partition is sparse data, not an unallocated address range.
- Each payload extent is length-checked and SHA-256 verified before any destination writes begin.
- A snapshot is complete or it is not usable for restore. Read failures must not silently produce a partial snapshot.

## Restore contract

1. Parse and validate the entire archive, reject duplicate/unsafe paths, and verify all payload hashes and ranges before writing.
2. Read the target's live partition table and running host app partition.
3. Require a compatible partition layout for every archived eligible partition. The initial implementation may require matching partition identity, offset, and size; incompatible layouts are rejected rather than guessed or silently remapped.
4. Recompute the target's protected ranges and reject any archived extent that overlaps the running MetaPass firmware, `cardid`, the partition table, or unallocated Flash.
5. Erase/write only eligible partition ranges. Restore omitted erased extents as erased bytes where necessary. Read back and verify the restored extents.
6. Never write the running MetaPass firmware, `cardid`, partition table, or unallocated gaps. Do not modify carve/allocation metadata outside the eligible partition list unless it resides inside an explicitly included partition.

## Version compatibility and page parity

- USB retains the historical v1.1 archive restore path unchanged for old archives.
- USB v2 creates and restores the new snapshot format.
- The phone embedded page supports v2 only and must use the same v2 manifest validation, protected-range policy, sparse extent semantics, and restore checks as USB.
- USB-only local firmware installation remains USB-only; other storage backup/restore semantics must match.
- The old v2 APP+DATA-only format must not be silently interpreted as a snapshot. Use a new manifest version/kind (or an unambiguous format discriminator) and reject ambiguous archives with an actionable message.

## Implementation sequence

1. Add pure shared snapshot-manifest/extent validation and tests (range overlap, protected partition exclusion, gaps, sparse erased extents, hash/length mismatch, incompatible layout, incomplete archives).
2. Add USB snapshot backup/restore on top of the existing chunked flash read/write path. Retain the legacy v1.1 branch.
3. Add device-side APIs for phone read/write of eligible partition ranges. The device, not the phone, resolves partition labels and validates absolute bounds against protected ranges; do not expose an unrestricted arbitrary-address write endpoint.
4. Route phone v2 through the same snapshot manifest/validation module and device-side guarded range API.
5. Update UI copy, docs, and handoff. Verify USB + mobile parity with browser tests, then have the local agent perform passport-sim and real-device tests.

## Explicit non-goals

- No NVS key/namespace parsing or merge.
- No per-app ownership inference for shared NVS.
- No cross-layout address remapping in the initial implementation.
- No resizing existing DATA partitions as part of backup/restore.
