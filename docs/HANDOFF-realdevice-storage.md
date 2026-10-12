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

passport-sim remains a useful boot/runtime regression layer, but the final firmware has already passed real ESP32-C3 S0-S6 validation. **Do not block merge solely because simulator was not rerun on the final firmware commit.**

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
- Until real byte-level backup is implemented, UI must warn that uninstall permanently deletes data and must not claim backup/restore is available.

### S5
Start a third install, upload only about one third, interrupt it, perform esptool soft reset, wait for service recovery, reissue prepare idempotently, resume from persisted offset, finalize and reboot.

This proves RAM-loss recovery. It does NOT prove physical power-loss durability.

### S6
Read partition table, carve store and DATA extent from flash. Byte-compare the DATA initial image against the flash bytes at the persisted DATA offset and calculate SHA-256.

## Failure evidence

On failure preserve:
- full stdout/stderr;
- UART log and tail;
- slots/status observations;
- executed commands and exit codes;
- partition/store/DATA dumps when safe;
- firmware SHA-256 and git commit;
- failure category.

Suggested categories:
ENV_MISSING, USB_PORT, USB_BUSY, FIRMWARE_FLASH, BOOT, AUTH, CAPACITY, CARVE, APP_UPLOAD, DATA_UPLOAD, DATA_RESUME, DATA_FINALIZE, ARCHIVE, REBOOT_RECOVERY, FLASH_VERIFY, POWER_LOSS_UNTESTED, UNKNOWN.

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

## Final code/test audit — required local-agent work

The final real-device-tested firmware corresponds to commit `5eed4b87a984a88485509a4ac2a0d577a506e55e`.

Before making further changes, the local agent must:

1. Review the final diff from `90c66f3` to the final commit and remove/flag unrelated changes.
2. Audit ownership of every field in `meta_carve_slot_t` and especially `meta_carve_flash_sync_states()`. Runtime reconstruction must not zero durable record-owned fields such as `play_id`.
3. Verify the regression test in `tests/test_meta_carve_flash.c` actually covers the `sync_states/play_id` bug.
4. Check `docs/storage-test-report-20261008.zh_CN.md` and the English report for traceability to final commit `5eed4b8...`; do not describe the final firmware as an uncommitted working tree on an older commit.
5. Verify the three fixes found during real-device testing: truncated HTTP status JSON, lost `play_id` during state sync, and the S6 hexadecimal flash-read length. Each must have a test or direct evidence.
6. Verify final static and firmware CI are green.
7. Do not rerun the entire real-device suite merely because simulator was not rerun. If the audit finds no new code issue, reuse the existing final-firmware S0-S6 evidence.
8. Keep physical power loss separate: soft reset proves RAM-loss recovery only. If no controlled power-cut setup exists, retain `POWER_LOSS_UNTESTED` and do not claim full P0-5 durability.

The audit output must answer: new defect? additional test needed? code/design change needed? final commit/firmware hash/CI/S0-S6 evidence traceable? physical power loss tested?

Only if a concrete defect is found should the agent enter code-change → regression-test → affected-layer rerun.

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
- passport-sim PASS;
- real-device S0-S6 PASS;
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
- NVS may contain Wi-Fi credentials and other device configuration. Reads require a valid paired session, an explicit user backup action, and an idle install transaction. Responses are marked no-store. Do not expose unauthenticated or arbitrary-partition reads.
- Restore still validates archive NVS bytes without writing them to the destination device. This API is read-only; it does not implement NVS restore.
- Cloud host/contract tests do not prove the real device partition label, size, or read behavior. On hardware, verify NVS info size, first/last chunk reads, out-of-range and unauthenticated rejection, archived `nvs.bin` size/hash, and that restore leaves destination NVS untouched.
