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

### S4
Delete the DATA-bearing slot. Verify DATA becomes ARCHIVED. Then delete the remaining slot with eraseData and verify count=0.

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
