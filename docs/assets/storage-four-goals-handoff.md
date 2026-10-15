# Handoff: Storage Four Goals — Implementation and Device Validation

English | [简体中文](storage-four-goals-handoff.zh_CN.md)

Date: 2026-10-14  
Repository: `alexwwang/meta-pass`  
Branch: `feat/storage`  
PR: [#1](https://github.com/alexwwang/meta-pass/pull/1)

This document hands the local agent a reproducible test plan for the four storage goals. It does not claim that incomplete implementation or unrun hardware tests have passed.

## Four goals and current boundaries

1. **The v2 snapshot path is connected in both pages; automated and hardware validation remain pending.** V2 means a whole-device allocated-storage snapshot, not a logical APP+DATA archive. USB retains historical v1.1 fixed-slot archive restore and supports v2 snapshot backup/sequential restore for dynamic layouts. The embedded phone page accepts v2 snapshots only. Both use full preflight, sequential partition erase/write, and independent full-partition readback verification, without transaction journals or automatic rollback. USB retains its unique local-firmware installation capability. Host mocks do not establish hardware success.
2. **Improve allocation while preserving fixed partitions.** Do not move `cardid` or `store`. Current pool ranges are `0x180000..0x356000` and `0x360000..0x7fe000`; a reservation cannot span a fixed partition. Report both total free bytes and largest aligned contiguous gap. If a declared minimum exceeds the largest gap, reject safely and show both largest contiguous free space and total free space. Both pages now display this fragmentation diagnostic.
3. **Validate DATA runtime I/O using the dedicated child firmware.** Build and install `tests/realdevice/data-child/`, then use `tools/realdevice/data_child_serial.py` over USB Serial/JTAG. Run HELLO, INFO, WRITE, READ, REBOOT, and READ again. The firmware must locate the DATA partition by the fixed `e2edata` label and must not accept arbitrary Flash addresses. A marketplace app that does not implement this protocol is not a valid substitute.
4. **Allocate DATA during app installation.** Verify alignment, declared minimum size, record capacity, and contiguous free space before writes. After installation verify APP/DATA ownership, actual DATA bytes, reboot persistence, uninstall cleanup, and reinstallation. Current allocation and some hardware installation coverage do not establish the entire lifecycle.

## Safety and branch rules

- Work only on `feat/storage`; do not develop on `main`, merge PR #1, force-push, or discard user changes.
- Run tests only on a dedicated device with no user data that needs preservation.
- Do not perform destructive actions against an unknown device or marketplace firmware.
- Never commit session tokens, pairing codes, device IPs, or sensitive serial identifiers.
- Save the tested commit, test firmware SHA-256, redacted commands, raw logs, and before/after snapshots.

## Stage A — host checks

From repository root:

```bash
./tools/validate.sh --static
./tools/validate.sh --all
```

If `--all` fails because ESP-IDF is unavailable, record that exact limitation; a passing static suite is not a firmware build. Check the latest Static checks and Firmware checks workflows for the tested commit.

## Stage B — non-destructive mobile browser smoke

Start the existing dev server and local Chromium/Chrome CDP using the repository's documented setup, then run:

```bash
node tools/realdevice/mobile_page_browser_smoke.mjs \
  --url 'http://127.0.0.1:4191/m' \
  --cdp-url 'http://127.0.0.1:9222'
```

Use the mobile module shell `/m`, not the USB page `/?mock=1`. Review viewport coverage, required controls, overflow, console errors, and failed requests.

## Stage C — dedicated child DATA read/write/reboot test

Read `docs/assets/mobile-page-data-e2e-test-firmware.zh_CN.md` and the CLI help before device writes. Build the child firmware in ESP-IDF 5.5.3 and verify its SHA-256. Confirm the exact target device and `e2edata` partition before erasing or writing anything.

Example commands (use the actual port and store evidence outside the repository unless sanitized):

```bash
python3 tools/realdevice/data_child_serial.py --port '<serial-port>' --command hello --evidence-file /tmp/data-hello.json
python3 tools/realdevice/data_child_serial.py --port '<serial-port>' --command info --evidence-file /tmp/data-info.json
python3 tools/realdevice/data_child_serial.py --port '<serial-port>' --command write --nonce '<32-hex-test-nonce>' --evidence-file /tmp/data-write.json
python3 tools/realdevice/data_child_serial.py --port '<serial-port>' --command read --nonce '<32-hex-test-nonce>' --evidence-file /tmp/data-read-before-reboot.json
python3 tools/realdevice/data_child_serial.py --port '<serial-port>' --command reboot --evidence-file /tmp/data-reboot.json
# Wait for the child firmware to boot, then verify the same record again.
python3 tools/realdevice/data_child_serial.py --port '<serial-port>' --command read --nonce '<32-hex-test-nonce>' --evidence-file /tmp/data-read-after-reboot.json
```

Require valid device-side readback, host SHA-256/CRC checks, matching nonce, and the same record after reboot. Do not continue if the child firmware cannot boot or the target partition cannot be identified. Do not move fixed partitions to bypass a failure.

## Stage D — mobile and USB lifecycle E2E

First read `docs/assets/mobile-page-storage-local-test-handoff.zh_CN.md` and the existing scripts' `--help` output. Confirm the safety baseline before running:

```bash
node tools/realdevice/mobile_page_e2e.mjs --help
node tools/realdevice/usb_web_e2e.mjs --help
node tools/realdevice/usb_web_e2e_multi.mjs --help
```

Use the repository's documented arguments and runtime driver; do not invent flags. Run mobile and USB suites separately on the dedicated test device. Cover app installation with and without DATA, ownership, cancellation without mutation, backup payload checksums, uninstall/release, reboot persistence, and reinstall. Stop if the baseline is not idle or the device identity is uncertain.

## Required acceptance matrix

| Scenario | Expected result |
|---|---|
| App without DATA | Installs without creating a spurious reservation |
| App with DATA | Reservation is owned by the same stable `play_id` |
| Invalid alignment, below-minimum size, duplicate label/subtype | Rejected before writes |
| Total free space is enough but contiguous gap is too small | Rejected; both values are reported |
| DATA record table full | Rejected without changing device state |
| Write/readback digest mismatch | Ownership is not published |
| Reboot | APP and DATA ownership plus byte digest remain unchanged |
| Cancel uninstall | No slot, reservation, or free-space changes |
| Uninstall | Only the target app and its owned DATA are removed; other apps remain |
| Phone v2 snapshot backup | Read live partition inventory; exclude running host, `cardid`, and unallocated gaps; verify each extent length and SHA-256 | Node tests + phone browser smoke + real-device ZIP verification (not yet run) |
| USB fixed-slot v1.1 archive | Preserve historical APP/NVS archive behavior | Contract tests + fixed-slot device check (not yet run) |
| Old APP+DATA v2 archive | Reject explicitly; never treat as the new snapshot kind | USB/phone contract tests |
| V2 snapshot restore | Full preflight, sequential partition erase/write, independent full-range readback including sparse 0xFF gaps, then separate reboot/reconnect check | Host tests + page logs + real-device report; report partition and relative offset on failure |
| USB dynamic v2 snapshot | Identify the MetaPass host uniquely from the ESP app descriptor; fail closed if ambiguous; use the live partition table for all relative I/O | Code/contract tests and real-device backup/restore report |

**Snapshot status (2026-10-15):** The v2 archive records the source MetaPass version and a conservative compatible-version range (currently the exact source version until a compatibility matrix is maintained). The device storage inventory reports the running MetaPass version. Both USB and phone pages call the shared sequential restore coordinator: validate archive and payload hashes, check version and live partition geometry, erase/write each allocated partition, and independently read back the full range including sparse 0xFF bytes. No transaction journal or automatic rollback is implemented. On write/readback failure, stop and recommend reflashing the source MetaPass version before retrying. Successful data readback is separate from bootability; the user must exit download mode/reboot and confirm reconnection. USB identifies the host via the ESP app descriptor and refuses ambiguous identification. Real-device restore and reboot validation remain pending; older APP+DATA reports are not evidence for the new snapshot path.

