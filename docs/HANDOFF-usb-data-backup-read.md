# Handoff | USB DATA Backup Read and APP+DATA Restore Device Test

## Baseline and status

- Repository: `alexwwang/meta-pass`; branch: `feat/storage`. Do not develop on `main`.
- Test the current branch HEAD and record `git rev-parse HEAD`. The initial handoff commit was `498943c6d5fbf85551864fa5c492f038e7398e56`; use the actual HEAD if it has advanced.
- The earlier implementation baseline had Static checks #1069 and Firmware checks #1041 passing at `5f785f5637f1a342e82ff41a7d497d93022eb1fd`. This does not prove browser serial reads or device restore work.
- APP and DATA backup use the existing v1.1-style `readFlashChunked()`: 32 KiB chunks and up to 8 retries. Do not introduce a second reader or change chunk size without reproducible evidence.
- Historical symptom: Chrome/macOS Web Serial appeared to stall after about 30 `readFlash` calls while backing up roughly 4.6 MiB DATA. `esptool.py` read roughly 4 MiB in about 18 seconds at 921600 baud. Reproduce and record this on the actual test environment; do not treat the historical observation as a current result.

## Stage A — host and simulator checks

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
git rev-parse HEAD
./tools/validate.sh
PASSPORT_SIM_DIR=../passport-sim ./tools/validate.sh --sim
```

Use the simulator command documented by the current checkout if it differs. Record command, exit code and logs. Missing/unstartable simulator is `ENV_MISSING` / `SIM_BOOT_FAILED`, not PASS. Do not proceed to destructive testing unless firmware and recovery paths are understood.

## Stage B — freeze the environment

Use the same Mac, Chrome, ESP32-C3 Passport, USB cable and baud-rate strategy as the earlier report when possible. Record:
- branch/commit, firmware SHA-256, ESP-IDF/esptool/Python/Node versions;
- macOS and full Chrome version, serial port, device IP;
- page URL/resource version and actual baud rate;
- current APP/slot/DATA listing and DATA label, play_id, offset and size.

Save the initial device listing. Do not erase/repartition or flash another firmware just to begin diagnosis. All addresses and lengths must come from current device metadata/API, never copied from a prior device.

## Stage C — read-only DATA backup

1. Open the USB service page, connect, enable Debug mode, and save page logs plus DevTools Console (screen recording is useful).
2. Back up exactly one APP+DATA group with a stable `play_id`. Do not run monitor/esptool concurrently on the same serial port.
3. Record each read's offset/length, duration, retries/recovery, total bytes, call count, and last log timestamp.
4. Save the ZIP locally and verify offline:
   - ZIP opens and manifest is a complete v2 single-group archive (`complete=true`);
   - no `missing_slots` / `missingSlots`;
   - every DATA reservation has a real payload whose length and SHA-256 match the manifest;
   - no DATA read, ownership-change, or incomplete-archive error occurred.

**PASS only if all checks pass.** A downloaded ZIP, an APP image, or device-side `flashMd5sum` alone is not evidence that DATA bytes were backed up.

If the read stalls after about 30 calls, wait at least 60 seconds and capture the complete page log, Console export/stack, last protocol/serial logs if available, last offset/length, completed byte count, browser responsiveness and final UI state. Classify as `USB_READ_TIMEOUT`, `USB_READ_PROTOCOL`, `PAGE_HANG` or `UNKNOWN`. Do not delete APP/DATA on a failed or incomplete backup.

## Stage D — delete, restore and persistence (only after Stage C PASS)

1. Copy the validated ZIP into the evidence directory and recheck manifest and all payload hashes.
2. Delete the APP using the normal USB page. Confirm its associated DATA is also removed and no orphan DATA remains. Do not erase partitions manually.
3. Restore that single APP+DATA group through the USB page.
4. Save the restored APP/DATA listing, including play_id, label, offset, size and transaction state.
5. Compare DATA byte-for-byte with the archive payload using a read-only Flash dump. Use only the current post-restore API/partition metadata for address and length. Save dump hash and comparison result. If safe dump is unavailable, mark the byte comparison incomplete; do not substitute metadata or MD5.
6. Reboot and confirm APP/DATA remain and the app can read the previous marker data. If the test firmware supports USB DATA commands, test write → read → reboot → read.
7. Confirm unrelated APP/DATA records and bytes remain unchanged.

PASS requires complete backup, successful cascade deletion, correct restore association, byte-for-byte DATA equality, reboot persistence, and no collateral changes.

## Read-only comparison and diagnosis

After browser testing has released the port, optionally use `esptool.py read_flash` on the same DATA range. Record exact command, baud, elapsed time and SHA-256; never run it concurrently with browser access.

- esptool PASS / browser FAIL: investigate Web Serial/esptool-js transport, ACK/response waits and call state.
- Backup completes but payload hash differs: investigate address/length, returned bytes and archive mapping.
- API metadata is wrong: investigate allocator/carve metadata before blaming transport.
- Restore metadata is right but bytes differ: investigate DATA write, offset, erase and restore transaction.
- Both readers fail: investigate hardware, serial link, flash state and device firmware.

Do not change 32 KiB chunks, add another reader, substitute `flashMd5sum` for raw DATA backup, or assume disconnect/connect guarantees a clean transport reset without reproducible evidence.

## Evidence and report

Create `tools/realdevice/logs/<timestamp>-usb-data-backup-read/` containing `report.md`, `report.json`, `environment.txt`, page and browser-console logs, commands and exit codes, ZIP and manifest summary, payload SHA-256 list, before/after device listings, and any safe read-only dump/hash artifacts. On failure include stack, last successful offset, failure category and evidence.

At minimum `report.json` must include `git_commit`, `git_branch`, `device_firmware_sha256`, `os`, `chrome_version`, `serial_port`, `baud`, `data_bytes`, `read_calls`, `read_retries`, `backup_duration_seconds`, `archive_sha256`, `payload_hashes_match`, `restore_byte_compare`, `reboot_persistence`, `overall_status`, `failure_category`, and `artifacts`.

Report simulator, browser byte backup, delete/restore, reboot persistence and physical power-loss testing separately. Physical power-loss testing is out of scope. On failure, deliver reproducible evidence and attribution first; do not guess at protocol changes.
