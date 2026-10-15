# v2 Storage Snapshot Backup/Restore — Real-Device Report (2026-10-15)

Language: [简体中文](storage-snapshot-restore-test-report-2026-10-15.zh_CN.md)

## Verdict

**PASS after fixes.** The handoff acceptance (§19–20 of `HANDOFF-realdevice-storage.zh_CN.md`) is met on the fixed tree: USB v2 snapshot backup/restore PASS, phone-page v2 snapshot restore PASS, USB v1 regression (host level) PASS, with reboot + reconnect verified after each restore and byte-level evidence. Testing also exposed and fixed three real defects at `b74e9da`.

## Environment

- HEAD under test: `b74e9da` (v2.1.0-1475) **plus worktree fixes** (firmware reports `v2.1.0-1475-gb74e9da-dirty`).
- Device: ESP32-C3 AI Passport, 8 MB Flash; full e2e-test image flashed (`build-e2e/FoloToy-AI-Passport-full-e2e.bin`, test-control build `CONFIG_META_E2E_TEST_CONTROL=y`), sha256 `6dd5f281…` (post-fix rebuild).
- WiFi re-provisioned after full flash by writing a minimal NVS image (namespace `metapass`, keys `sta_ssid`/`sta_pass`) at 0x9000; pairing via `E2E SESSION_RESET` (token redacted, `/tmp/token.txt`).
- Host: macOS 15.7.4, Chrome 155.0.8059.40, Node v26.3.0, esptool 5.4.0, ESP-IDF 5.5.3.

## Defects found and fixed during this round

1. **Snapshot silently omits all child DATA partitions (critical).** The launcher-mode materialized partition table only materializes DATA partitions for the *active* child (`meta_pt_from_carve_active`); both snapshot inventories enumerated only that table, so child DATA was never captured. Demonstrated on hardware: a written and device-verified DATA record (nonce record, sha256 `b361df96…`) was destroyed by post-backup corruption and the restore did not bring it back — with no warning. **Fix**: device `storage/info` now merges carve DATA records into the inventory (exact label/type/subtype/offset/size identity, skipping already-materialized entries), and `storage/read|erase|write` resolve identities against carve records via `esp_flash_*` when the table has no match; the USB page merges `slotModel.carve.data` into `readUsbStorageInventory()` the same way. Reference: host-side `materializeTable(carve)` in `dynslot-record.js` (which always included DATA).
2. **USB snapshot page never loaded (dev server).** `install-slot.html` imports `storage-snapshot.js`/`storage-snapshot-restore.js`, but the dev-server static whitelist lacked both → 404, page module dead. Fixed in `tools/install-slot/server.mjs`.
3. **`assertLoader is not defined` (ReferenceError).** `readUsbStorageInventory()` called `assertLoader`, previously a function-scoped local inside `commitCarve`. Hoisted to module scope in `install-slot.html`.

## Results (post-fix firmware, group = data-child `SNAPSHOT-E2E`, play_id 2147483648)

### USB v2 snapshot backup (#3, `usb-backup3-fixed/`)

- One click on `#btn-backup`, PASS in 36.9 s; archive 214,093 B, sha256 `237b2037…`.
- Manifest partitions: nvs, phy_init, ota_0 (43 extents), store (3), otadata, **e2edata (1 extent = the 4 KiB DATA record sector at 0x1b0000)** — DATA coverage confirmed.

### Phone-page v2 snapshot restore (`phone-restore-fixed/`)

- Scope: per user decision, testing targeted this round's new backup/restore functionality; the phone page's full load/install flow was not re-regressed (module-level entry via the storage-management panel). The init/pair/listing path was exercised as a prerequisite and worked.
- After garbage-writing ota_0 + e2edata with 0xAA: confirm dialog listed 6 partitions to overwrite (5 pre-fix + e2edata), restore PASS in 6.9 s.
- Final UI state: "snapshot data written; reboot the device and confirm MetaPass starts and reconnects; this step is not yet completed automatically" (`warn` class) — written ≠ boot-validated, correct semantics; no `rebootAndReconnect` adapter, so boot validation is manual per handoff.
- Manual DTR/RTS reboot → MetaPass booted, LAN reconnected, slots listing fully restored (count=1, slot0 @1572864+176128, e2edata @1769472+65536).
- Byte-level evidence: esptool dumps of 0x180000+176128 and 0x1b0000+65536 vs snapshot payloads — **44/44 extents byte_equal, including the e2edata DATA record** (`t6-phone-restore-fixed-byte-compare.json`).

### USB-page v2 snapshot restore (`usb-restore-fixed/`)

- Same garbage mutation → USB restore PASS in 10.2 s, same warn state (reboot pending).
- Manual reboot → boot + reconnect OK, slots restored.
- **44/44 extents byte_equal including e2edata** (`t6-usb-restore-fixed-byte-compare.json`).

### Failure-boundary observations (pre-fix, kept as evidence)

- Restoring a snapshot onto a **different table shape** (group uninstalled → placeholders back in table) is rejected pre-erase: "snapshot does not cover every eligible allocated partition" — fail-closed before any Flash write, as specified.
- Uninstall cleanup raced briefly ("pending uninstall cleanup failed; retry" for ~60 s); the fail-closed gate held and the idempotent recovery completed after reboot (`carve loaded: seq=3 slots=0`). No data-loss symptom beyond the erase in progress.

### USB v1 compatibility regression

- Host tests `test-data-byte-archive.mjs`, `test-backup-data.mjs`, `test-slot-backup.mjs` all PASS (v1 manifest rejection of DATA archives, NVS payload path, v1 restore data-path contract).
- Real-device v1 restore: **NOT RUN** — the device is dynamic-mode; v1 restore requires a fixed-slot device. Per handoff: host test + explicit NOT RUN, no false PASS.

## Not covered

- Physical power-loss testing (`POWER_LOSS_UNTESTED` per handoff; not required this round).
- Full-flash readback after restore (deliberately removed by design; byte-level spot verification via independent esptool dumps used instead).
- Phone-page backup leg used the repo module served via local route interception (device shell loads `phone-install.js` from a CDN deploy that lags repo HEAD); restore logic under test is current code.

## Evidence

`tools/realdevice/logs/20261015-snapshot-restore/`: `environment.txt`, inventories, per-step rundirs (`usb-backup`, `usb-backup2`, `usb-backup3-fixed`, `usb-restore`, `usb-restore-fixed`, `phone-restore`, `phone-restore-fixed`) with driver logs, page logs, result JSON, screenshots, dumps and `t6-*-byte-compare.json`. UART logs in `/tmp/uart-*.log` (tokens redacted).

## Fixes to commit

- `main/meta_store_install.c` — carve DATA identity fallback for storage snapshot inventory/read/erase/write.
- `install-slot/install-slot.html` — module-scope `assertLoader`; merge carve DATA into snapshot inventory.
- `tools/install-slot/server.mjs` — static whitelist for snapshot modules.
- `tests/test_storage_snapshot_device_contract.py`, `tests/test_usb_backup_segmented_read.py` — contract assertions for the DATA merge.
- New drivers: `tools/realdevice/usb_snapshot_backup_once.mjs`, `usb_snapshot_restore_once.mjs`, `phone_snapshot_restore_once.mjs`.
