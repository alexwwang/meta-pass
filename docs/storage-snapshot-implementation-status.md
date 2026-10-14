<p align="right">
  <strong>English</strong> · <a href="storage-snapshot-implementation-status.zh_CN.md">简体中文</a>
</p>

# Storage Snapshot Implementation Status (2026-10-14)

Branch: `feat/storage`.

## Implemented foundation

- `install-slot/storage-snapshot.js`: v2 snapshot manifest validation, partition geometry/protection/extent checks, ZIP generation and parsing, SHA-256 verification, and a non-mutating restore-plan builder.
- `main/meta_store_install.c`: authenticated, same-origin read-only partition inventory and partition-relative read APIs. Reads of the running host partition and `cardid` are rejected; callers cannot submit absolute Flash addresses as read targets.
- `install-slot/phone-install.js`: phone storage management now offers whole-device snapshot backup with a sensitive-data warning. Restore currently performs archive-integrity and target-layout preflight only; it does not write Flash.
- English and Chinese design/status documentation are present.
- Automated coverage has been added to `tests/test_storage_snapshot.mjs`, `tests/test_phone_install.mjs`, and `tests/test_storage_snapshot_device_contract.py`, and registered in `tools/validate.sh`.

## Not complete

- The USB page has not switched to the new snapshot archive. The historical USB v1.1 backup/restore path remains unchanged. Before USB snapshot support can be implemented safely, it must reliably obtain the device's currently running host firmware partition; inferring it from the partition table or guessing from OTA state is not sufficient.
- Phone snapshot restore performs no writes yet. The device currently exposes read-only APIs. A bounded, verifiable transactional restore protocol and a clear rollback strategy for write failures/disconnects must be implemented before enabling restore writes.
- Old APP+DATA v2 archives must not be treated as the new snapshot format. The existing USB v2 APP+DATA flow still needs migration or an explicit compatibility policy.
- The latest GitHub Actions Static checks failed at the documentation language-link checks. Those links have been fixed and the missing English status document added; CI must be checked again.
- Final static tests, ESP-IDF firmware build, and USB/phone browser regression have not yet been confirmed. Real-device testing has not been run; after code and CI checks, hand off passport-sim and hardware regression to the local agent.

## Safety requirements

Before any restore write, validate the entire archive, every payload's length and SHA-256, the target device's live partition layout, and protected ranges. A restore plan must address only explicitly eligible partitions and must never expose an arbitrary-address write API. Old APP+DATA v2 archives must not be mistaken for `meta-pass-storage-snapshot`; historical USB v1.1 archives remain on their existing path.
