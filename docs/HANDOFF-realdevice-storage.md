[Chinese](HANDOFF-realdevice-storage.zh_CN.md)

# Handoff | feat/storage Real-device validation

This file is the executable handoff for the next local coding agent.

## Goal

Validate the dynamic APP + DATA storage installation path on a real ESP32-C3 Passport, collect reproducible evidence, classify failures, and feed actionable findings back into code changes.

Work only on branch `feat/storage`; never develop directly on `main`.

## Test layers

1. Host TDD and firmware build.
2. passport-sim using the real ESP32-C3 QEMU path.
3. Real Passport hardware over USB Serial/JTAG + LAN HTTP.

A real power-loss test is separate from a soft reset. Never report soft reset as power-loss PASS.

## passport-sim regression (optional, not a merge blocker)

passport-sim remains a useful boot/runtime regression layer, but historical S0-S6 evidence does not cover the subsequently added mobile NVS backup API and S1b checks. Do not treat historical hardware results as validation of the current HEAD; track simulator and current-HEAD real-device validation separately.

If a local agent has the simulator environment, run:

```bash
PASSPORT_SIM_DIR=../passport-sim ./tools/validate.sh --sim
```

If it fails, classify image/boot/display/CPU/button/TEST_HARNESS first. Only change meta-pass when evidence points to a real regression relevant to this branch.

Historical simulator PASS is background evidence only; unless rerun against the final firmware commit, do not report it as final-commit simulator PASS.

## Real-device prerequisites

- ESP32-C3 AI Passport with 8 MB flash.
- Data-capable USB-C cable.
- USB Serial/JTAG device visible to the host.
- ESP-IDF 5.5.3.
- Python 3, Node.js >= 20, esptool, pyserial.
- Passport and host on the same LAN.
- Device IP and USB port.

Check:

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py --version
python3 -m serial.tools.list_ports -v
```

ESP32-C3 provides a USB Serial/JTAG controller that supports serial console, flashing, and JTAG through the USB connection. Use `idf.py monitor` for firmware logs. See Espressif's USB Serial/JTAG and IDF Monitor documentation.

## Real-device command

Preferred entry:

```bash
python3 tools/realdevice/run_smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX
```

Clean-device run:

```bash
python3 tools/realdevice/run_smoke.py \
  --ip <PASSPORT_IP> \
  --port /dev/cu.usbmodemXXXX \
  --fresh
```

The wrapper executes `smoke.py` and creates one evidence directory under:

```
tools/realdevice/logs/<timestamp>/
```

Expected artifacts include command.txt, stdout.log, stderr.log, uart.log, HTTP observations, report.json and report.md.

## Functional stages

### S0
Build/flash/start. Verify no panic, watchdog loop, or boot loop and that the LAN service is reachable.

### S1
Capture pairing information from UART, pair through HTTP, verify token consistency, then prefer persistent session/token recovery.

### S2
Install APP + DATA. Verify:
- allocator proposal comes from the Node dynslot implementation;
- prepare succeeds;
- APP upload completes;
- DATA prefix upload establishes the expected checkpoint;
- status reports that DATA offset;
- only the DATA suffix is uploaded;
- DATA becomes done;
- finalize succeeds;
- reboot preserves one installed slot.

### S3
Install a second APP slot and verify two slots survive reboot without damaging the first installation.

### S4 Cascade-delete APP + DATA

- Delete an APP with multiple DATA labels.
- The device enumerates all DATA belonging to that APP; UI offers neither standalone DATA deletion nor data retention.
- After success, APP and all associated DATA records are absent, no extents remain referenced, and free-space accounting matches released extents.
- Records and bytes for other APPs/DATA remain unchanged.
- If erase or persistent commit fails, do not report success or reclaim extents that remain referenced.
- Interrupt deletion and reboot; the durable transaction must recover idempotently, and image scanning must not resurrect the deleted APP.
- The UI must clearly explain the deletion/archival behavior before confirmation. The user must not be led to believe that an APP archive automatically guarantees recovery of associated DATA; verify the actual DATA lifecycle and archive contents in the report.

### S5
Start a third install, upload only about one third, interrupt it, perform esptool soft reset, wait for service recovery, reissue prepare idempotently, resume from persisted offset, finalize and reboot.

This proves RAM-loss recovery. It does NOT prove physical power-loss durability.

### S6
Read partition table, carve store and DATA extent from flash. Byte-compare the DATA initial image against the flash bytes at the persisted DATA offset and calculate SHA-256.

## Failure evidence

On failure preserve:
- full stdout/stderr;
- UART log and tail (persisted UART logs must redact session tokens; pairing still uses the unredacted in-memory line);
- slots/status observations;
- executed commands and exit codes;
- partition/store/DATA dumps when safe;
- firmware SHA-256 and git commit;
- failure category.

Suggested categories:
ENV_MISSING, USB_PORT, USB_BUSY, FIRMWARE_FLASH, BOOT, AUTH, NVS_API, CAPACITY, CARVE, APP_UPLOAD, DATA_UPLOAD, DATA_RESUME, DATA_FINALIZE, ARCHIVE, REBOOT_RECOVERY, FLASH_VERIFY, POWER_LOSS_UNTESTED, UNKNOWN.

## Attribution

- Host failure -> logic/test issue.
- Firmware build failure -> compiler/linker/size.
- Simulator boot failure -> image/partition/bootloader.
- Simulator pass + real boot failure -> hardware, flash, image or real-device difference.
- HTTP pass + upload failure -> installer/storage protocol.
- Metadata correct + flash bytes wrong -> DATA write/offset/erase.
- Reboot failure -> durable carve record/boot recovery/runtime table.
- Soft-reset pass + physical power-loss failure -> durability/commit ordering.
- USB no logs -> cable/driver/port/monitor ownership.
- Panic/watchdog -> firmware runtime.
- Only fresh passes -> persistent state/migration/reclaim.

Do not change code before collecting evidence and classifying the failure.

## Current-branch validation discipline

The fixed “final real-device-tested commit” and S0-S6 PASS statements in older handoff text are historical evidence only; they do not certify the current `feat/storage` HEAD. The branch has since added restore-transaction fault injection, USB v2 archive/NVS semantics, the mobile system-NVS read-only API, and real-device S1b checks. Validate these changes against the current commit.

Before testing, the local agent must:

1. Confirm a clean worktree and record `git rev-parse HEAD`, `git status --short`, and firmware SHA-256.
2. Run `./tools/validate.sh` and build the firmware at the current HEAD, recording commands and exit codes.
3. If hardware is available, run the wrapper below. S1b must pass before any install/removal mutation.
4. Ensure the report's git commit matches the tested worktree. If firmware was built from a different commit, record that separately.
5. On failure, preserve stdout/stderr, UART, HTTP observations and stage number before attributing or fixing; rerun affected tests after each fix.
6. Soft reset is not physical power loss. If actual power-cut testing was not performed, report `POWER_LOSS_UNTESTED`.

Never present historical S0-S6 results, old simulator results, or host fake-device tests as current-HEAD real-device PASS.

## Current smoke coverage and NVS stage

`tools/realdevice/smoke.py` runs S1b after pairing and before install/removal:

- Unauthenticated NVS info/read requests must return HTTP 401.
- Authenticated requests query the fixed system `nvs` partition and read small first/last chunks.
- Out-of-range reads must return 416; zero-length and over-4-KiB reads must return 400.
- Successful reads must include `Cache-Control: no-store`.
- Logs include status and byte lengths only, never raw NVS bytes.
- Partition size must be within 1 byte to 1 MiB.

The wrapper `tools/realdevice/run_smoke.py` generates `report.json` and `report.md`. Green CI only proves the automated checks actually included in CI; S1b and archive/restore behavior still require execution on the real device.

## Report

report.json must contain at least:
- timestamp/run id;
- git branch/commit;
- firmware SHA-256;
- host and tool versions;
- serial port/IP;
- fresh flag;
- stage results;
- overall status;
- failure category;
- artifact paths;
- whether soft reset was tested;
- whether physical power loss was tested.

report.md must summarize the same information for a human and explicitly list untested items.

## Completion criteria

Do not claim complete hardware validation until:
- Host PASS;
- real-device stages applicable to the current HEAD PASS;
- passport-sim PASS when the simulator environment is available and the run is part of the declared validation scope; otherwise record it as not run, not PASS;
- DATA byte-level flash verification PASS;
- reboot recovery PASS;
- evidence package complete.

If physical power loss was not actually performed, report POWER_LOSS_UNTESTED.


## 15. Added 2026-10-12: restore failure injection and uncertain finalize state

`tools/install-slot/test-data-restore-transaction.mjs` is registered in `tools/validate.sh` and covers:

- APP write failure before commit: commit must not run; temporary staging cleanup must be attempted.
- Commit request timeout: treat the result as uncertain and do not clean staging.
- Simulated durable commit followed by a transport timeout: do not clean extents that may already be referenced.
- Ownership/geometry readback failure after a successful commit response: do not clean possibly published data; preserve state for recovery inspection.

The DATA ZIP parser also now tracks local-record byte ranges and rejects central-directory entries that alias or overlap the same local record. `tools/install-slot/test-data-archive-zip.mjs` includes a malicious central-offset aliasing regression test.

The review also found that `restoreDataArchive()` unconditionally called `cancel()` after any install failure. That could treat an accepted finalize with a timed-out terminal-state poll as safe to roll back. The implementation now marks such outcomes `commitUncertain` and skips cancellation; rollback remains enabled when the device explicitly reports terminal `failed`. A thrown transport error from the finalize request is also treated as uncertain because the request may have reached the device; the user must inspect device state before retrying. Integration tests cover timeout, transport exception, and explicit terminal failure.

These tests still do not prove real Flash atomicity or physical power-loss durability. After cloud CI passes, the local agent must inspect the real prepare/session/chunk/finalize path on target firmware. Never present fake-device tests as real-device evidence.

## 16. Added 2026-10-12: USB v2 archive scope and NVS restore semantics

- In dynamic-storage mode, USB backup now accepts exactly one selected APP slot per archive, matching the mobile page's single APP+DATA archive/restore behavior. This prevents creating a multi-APP v2 archive that the restore path cannot consume.
- A v2 archive may retain and validate the automatically backed-up global NVS bytes, but v2 APP+DATA restore does not write global NVS; the destination device's existing NVS remains unchanged. Mobile success copy and USB preflight/result copy explicitly disclose this.
- Legacy v1 archives keep their sequential APP/NVS restore compatibility path. Verifying but not applying v2 NVS avoids overwriting global NVS outside the APP+DATA carve transaction; it does not prove atomic NVS restoration.
- Host tests cover the USB single-group backup gate and successful APP+DATA restore from a v2 archive that also contains NVS, while preserving destination NVS. Real-device validation must still verify restored DATA bytes and device state.

## Mobile system-NVS backup

- The mobile page now reads only the fixed system partition labelled `nvs` through paired-session-authenticated `/api/install/nvs/info` and `/api/install/nvs/read`. Clients cannot choose a partition label or raw flash address.
- Each read is limited to 1–4096 bytes and checked against partition bounds. The client reads in 4 KiB chunks and stores the exact bytes as `nvs.bin`, with size and SHA-256 in the archive manifest.
- Backup ZIP files are unencrypted. NVS may contain Wi-Fi credentials and other device configuration; do not upload or share these archives. Reads require a valid paired session, an explicit user backup action, and an idle install transaction. Responses are marked no-store. Do not expose unauthenticated or arbitrary-partition reads.
- Restore still validates archive NVS bytes without writing them to the destination device. This API is read-only; it does not implement NVS restore.
- Cloud host/contract tests do not prove the real device partition label, size, or read behavior. On hardware, verify NVS info size, first/last chunk reads, out-of-range and unauthenticated rejection, archived `nvs.bin` size/hash, and that restore leaves destination NVS untouched.

## Real-device NVS API safety checks

`tools/realdevice/smoke.py` now runs S1b after pairing and before any install/removal mutation:
- Requests without a session token must receive 401 for both NVS info and read.
- The paired token can query the fixed system NVS partition size and read small first/last chunks; the test logs status and lengths only, never raw NVS bytes.
- An out-of-range read must return 416.
- Successful read responses must include `Cache-Control: no-store`.
- Partition size must be greater than zero and no more than 1 MiB; the actual device value comes from the endpoint.

S1b is a new real-device assertion. Cloud CI validates the code/contract but does not substitute for running the smoke test on hardware.


## Snapshot restore status (2026-10-15)

This section supersedes any older pending handoff checklist for the v2 whole-device snapshot restore task. See the [real-device report](storage-snapshot-restore-test-report-2026-10-15.md).

### Completed

- Branch: `feat/storage`; fixes committed as [`7beba51d`](https://github.com/alexwwang/meta-pass/commit/7beba51d8d529ca4eb62a1c98d6da26dbda6b54f).
- The report records `./tools/validate.sh` full PASS (static, host, firmware); CI results are not a substitute for hardware evidence.
- USB v2 snapshot backup PASS in 36.9 s; the archive includes the child DATA `e2edata` record.
- USB v2 restore PASS; manual reboot confirmed MetaPass startup/reconnect; 44/44 extents byte-matched the archive payload.
- Phone-page v2 restore PASS; manual reboot confirmed MetaPass startup/reconnect; 44/44 extents byte-matched the archive payload.
- A snapshot with a different partition-table shape was rejected before the first erase.
- USB v1 host tests recorded in the report passed. Real-device v1 restore is NOT RUN because the tested dynamic-mode device does not support that legacy restore scenario.

### Evidence boundaries and regression policy

- The tested firmware was built from `b74e9da` plus then-uncommitted worktree fixes and reported a `-dirty` version. Those fixes were subsequently committed as `7beba51d`; the report preserves the actual source provenance and does not claim the dirty firmware was built directly from the commit SHA.
- This round exercised the phone page's v2 restore path only. The phone-page backup leg was not rerun on hardware, and the device's deployed CDN version lagged the repository branch. Do not describe the phone page's complete backup/restore pair as hardware-validated.
- Physical power-loss testing and full-partition readback are not required for this scope. Acceptance is reboot, normal MetaPass startup/reconnect, plus independent byte comparison of all 44 recorded extents.
- Regress only paths affected by a change. Do not rerun USB v1 archive tests when v1-specific code and its shared call paths are untouched; run targeted v1 checks if a future change can affect them.
- The old pending checklist above is no longer an execution request for this completed round. For future code changes, add only tests justified by the changed impact surface and record new evidence in the dedicated report.
