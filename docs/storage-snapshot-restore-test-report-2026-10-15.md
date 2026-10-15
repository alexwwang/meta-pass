# v2 Storage Snapshot Backup/Restore — Real-Device Report (2026-10-15)

Language: [简体中文](storage-snapshot-restore-test-report-2026-10-15.zh_CN.md)

## Verdict

**v2 snapshot real-device acceptance PASS within the scope recorded here.** USB v2 snapshot backup/restore and phone-page v2 snapshot restore passed. Both restore paths were followed by manual reboot, successful MetaPass startup/reconnect, and byte-for-byte comparison of all 44 extents. A different partition-table shape was rejected before the first erase. USB v1 real-device restore was not run; v1 compatibility claims are limited to the recorded host tests. The three defects found during testing were fixed in [`7beba51d`](https://github.com/alexwwang/meta-pass/commit/7beba51d8d529ca4eb62a1c98d6da26dbda6b54f).

## Environment

- Tested firmware source: `b74e9da` (v2.1.0-1475) plus then-uncommitted worktree fixes; firmware reported `v2.1.0-1475-gb74e9da-dirty`. Those fixes were subsequently committed to `feat/storage` as [`7beba51d`](https://github.com/alexwwang/meta-pass/commit/7beba51d8d529ca4eb62a1c98d6da26dbda6b54f). This report records the actual test source state and does not claim the dirty firmware was built directly from that commit.
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

### USB v1 compatibility boundary

- The host tests recorded in this run — `test-data-byte-archive.mjs`, `test-backup-data.mjs`, and `test-slot-backup.mjs` — all PASS for v1 DATA-archive rejection, the NVS payload path, and the v1 restore data-path contract.
- Real-device v1 restore: **NOT RUN** — this is a dynamic-mode device; v1 restore requires a fixed-slot device. Host PASS is not reported as a hardware PASS.
- Future regression policy: rerun v1 tests only when a change touches v1-specific archive/restore code or a shared path that can affect it; do not rerun unaffected v1 paths mechanically.

## Not covered

- Physical power-loss testing (`POWER_LOSS_UNTESTED` per handoff; not required this round).
- Full-flash readback after restore (deliberately removed by design; byte-level spot verification via independent esptool dumps used instead).
- Phone-page backup leg used the repo module served via local route interception (device shell loads `phone-install.js` from a CDN deploy that lags repo HEAD); restore logic under test is current code.

## Evidence

`tools/realdevice/logs/20261015-snapshot-restore/`: `environment.txt`, inventories, per-step rundirs (`usb-backup`, `usb-backup2`, `usb-backup3-fixed`, `usb-restore`, `usb-restore-fixed`, `phone-restore`, `phone-restore-fixed`) with driver logs, page logs, result JSON, screenshots, dumps and `t6-*-byte-compare.json`. UART logs in `/tmp/uart-*.log` (tokens redacted).

## Commit status

The device-side DATA inventory/read/erase/write fix, USB inventory and `assertLoader` fixes, dev-server module allowlist, contract tests, and real-device drivers are committed on `feat/storage` in [`7beba51d`](https://github.com/alexwwang/meta-pass/commit/7beba51d8d529ca4eb62a1c98d6da26dbda6b54f). No code changes from this report remain pending commit.
