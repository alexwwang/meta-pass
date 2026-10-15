<p align="right">
  <a href="storage-snapshot-restore.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Storage snapshot restore: design and test handoff

Status: implementation in progress on `feat/storage`. This document describes the intended v2 snapshot behavior; it is not evidence of a completed real-device test.

## Scope and page compatibility

- The archive is a raw snapshot of the current eligible allocated storage partitions, including NVS and allocated storage/data partitions.
- The running MetaPass host firmware partition and `cardid` are excluded. The live partition table remains authoritative; do not relocate `cardid`, invent fixed slot addresses, or write to unallocated Flash gaps.
- USB service page retains its historical v1 archive restore path. The v2 snapshot path is separate. The mobile embedded page supports v2 only and does not gain v1 archive compatibility.
- Restoring an existing DATA partition's size is out of scope. Partition resizing is not part of snapshot restore.

## Archive metadata and compatibility

Every v2 manifest records:

- `meta_pass.system_version`: the exact MetaPass version that produced the snapshot.
- `meta_pass.compatible_versions.min_inclusive` and `max_inclusive`: the inclusive MetaPass version range allowed to consume the archive.

The source version must lie inside the declared range. Snapshot capture currently writes a conservative exact-source range (min=max=source version), because there is no validated compatibility matrix yet. This prevents claiming cross-version compatibility without evidence. A future wider range must be supported by an explicit, tested compatibility policy, not inferred from the archive format number alone.

Before any Flash mutation, restore validates archive structure, every payload hash, target device version, live partition identities/layout, protected ranges, and the full restore plan. Missing or unparseable device version is a hard preflight failure.

## Restore algorithm

1. Parse the complete archive and verify all payload hashes.
2. Read the live partition inventory and require a valid MetaPass version.
3. Check archive compatibility and resolve each target partition by live identity (label/type/subtype plus live geometry). The device adapter resolves all actual Flash access through that live identity; the restore coordinator passes partition-relative offsets only.
4. For each eligible partition in order, erase that partition only, write the declared extents, then independently read back the entire partition in bounded 4096-byte chunks.
5. During readback, undeclared sparse gaps are expected to contain `0xFF`. Compare every byte; stop at the first mismatch.
6. If any erase/write/readback step fails, stop immediately. Do not attempt rollback, transaction recovery, or automatic repair.
7. Only after every partition has passed readback is data restore considered verified. Bootability is a separate check: exit ROM/download mode, restart the device, and confirm MetaPass starts and can reconnect. A reboot failure does not retroactively mean the byte-level readback failed.

## User-facing failure handling

- **Preflight failure:** state that no Flash writes were started and identify the archive, firmware-version, or live-layout issue.
- **Erase/write failure:** state that restore is incomplete, name the partition and relative offset, and direct the user to reflash the archive's source MetaPass version before retrying.
- **Readback failure:** state that restore is incomplete/inconsistent; report partition, relative offset, expected byte and observed byte when available; recommend reflashing the source MetaPass version and retrying.
- **Boot/reconnect failure after successful readback:** explicitly say data readback passed but bootability validation did not; recommend reflashing the source MetaPass version.
- Do not exaggerate likely user-data loss. The recovery path is to restore the matching MetaPass system version and retry the snapshot.

The USB page's serial session is in ROM/download mode during Flash operations, so post-restore boot validation is currently a guided manual step: exit download mode/restart, then confirm the device can reconnect. Do not report it as automatically validated when no reconnect check was executed.

## Required verification

Automated tests should cover:

- compatible version accepted; incompatible or missing version rejected before erase/write;
- malformed archive and payload hash failures before writes;
- live partition identity/layout mismatch and protected host/cardid ranges;
- sequential erase/write and full independent readback;
- sparse gaps expected as `0xFF`;
- exact partition-relative offset for write and readback failures;
- no later partition operations after first failure and no rollback;
- successful data verification distinguished from failed reboot/reconnect validation;
- USB v1 compatibility remains intact, while the mobile page only accepts v2 snapshots.

Run `tools/validate.sh --static` / the repository CI static checks. Real hardware testing is a separate local-agent task; do not claim it from mocks or GitHub CI.

## Local-agent real-device handoff

1. On a disposable test device, record the running MetaPass version and the live partition inventory.
2. Capture a v2 snapshot using the USB page; confirm manifest version metadata and payload hashes.
3. Verify restore refuses a deliberately incompatible/missing-version fixture before any erase/write (use mocks/automated tests for this case; do not falsify real device metadata).
4. Restore the captured snapshot on the same compatible firmware version. Capture UI logs and confirm every eligible partition reaches readback verification.
5. Exit download mode/restart, then verify MetaPass boots and reconnects. Record this separately from byte-level readback.
6. Do not run destructive fault-injection against user data. For offset mismatch, write failure, or reboot failure, use automated mocks unless a dedicated disposable device and explicit test image are available.
7. Report firmware version, device/partition inventory, archive manifest metadata, test steps, logs, outcome, and any mismatch as partition + relative offset. Do not move `cardid` or manually substitute absolute Flash addresses.
