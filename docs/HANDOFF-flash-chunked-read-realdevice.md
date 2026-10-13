# Handoff | feat/storage Flash Chunked-Read Real-Device Validation

Date: 2026-10-13  
Target branch: `feat/storage`  
Test the latest `origin/feat/storage` HEAD at execution time. Do not reuse PASS results from an older commit.

Language: [简体中文](HANDOFF-flash-chunked-read-realdevice.zh_CN.md)

## 1. Goal

Verify that long Flash reads remain reliable when split into bounded chunks with periodic serial-session recovery. The target is **Flash reading and read-back verification**, not chunked writing.

Expected implementation:
- Each Flash read is at most 32 KiB.
- After each 512 KiB of cumulative reading, disconnect and reopen the current serial transport, clear its receive buffer, resynchronize, then continue.
- APP and DATA read-back both use the shared chunked-read/byte-comparison module.
- Short reads, invalid lengths, byte mismatches, and reconnect/sync failures must fail closed.
- Existing Flash erase/write behavior remains unchanged.

## 2. Entry gates

Run and record actual output:

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
git status --short
git rev-parse HEAD
./tools/validate.sh
```

Confirm Static checks and Firmware checks pass for the current HEAD. Record firmware SHA-256, device ID (redacted if needed), OS/browser/Node versions, ESP-IDF version, serial port, and page entry point. If the worktree is dirty, commit/build/test state is wrong, or host tests fail, stop before destructive device operations.

## 3. Test sequence

### R0 — Baseline
Record device APP/DATA inventory and Flash-space state. Verify device, serial connection, pairing, and page version. Do not move cardid, hardcode Flash addresses, or alter the fixed partition layout.

### R1 — APP long read-back
1. Use the build under test to install or select an approved test APP image.
2. Trigger post-write APP read-back verification and retain page/serial logs.
3. Verify each read is no larger than 32 KiB and each 512 KiB boundary produces disconnect → connect → sync.
4. Confirm contiguous address coverage with no gaps or duplicates and a successful byte-for-byte comparison.
5. Use an APP image larger than 512 KiB, preferably several MiB, to exercise multiple session boundaries.

### R2 — DATA long read-back
1. Trigger post-write DATA read-back in an approved APP/DATA scenario.
2. Verify chunk sizes, contiguous offsets, 512 KiB session boundaries, and byte comparison.
3. Use multi-MiB DATA to exercise multiple reconnects; record duration, timeouts, and any manual recovery.
4. Confirm the APP/DATA inventory and partition ranges remain unchanged.

### R3 — Fail-closed behavior
Host tests must show that short reads, byte mismatches, read exceptions, and reconnect/sync failures return explicit failures. If a real-device failure occurs, preserve evidence; do not use `?noverify` or skip verification to report PASS.

### R4 — Backup/restore regression (only when safe)
1. Verify a usable backup exists and follow every human-confirmation gate in the existing backup/restore handoff.
2. Check that long APP/DATA read-back no longer hangs during sustained reads.
3. If restoring, separately record writes, read-back verification, transaction commit, and final inventory. Successful writes do not prove read-back verification.
4. After each install/restore involving real-market firmware 28, pause for explicit human confirmation that DATA can actually be read, written, and read back correctly. Otherwise keep status `BLOCKED_WAITING_FOR_HUMAN_DATA_CONFIRMATION`.

## 4. Pass criteria

- Both APP and DATA long reads complete; every read chunk is at most 32 KiB.
- Logs prove serial reconnect and resynchronization at every 512 KiB boundary, followed by successful continuation.
- Read ranges are contiguous, with no short reads or gaps; byte comparison and applicable hashes pass.
- Multiple reconnects cause no permanent hang or unexplained timeout, and the page reports a definitive outcome.
- Existing erase/write flow was not inadvertently changed into a new chunked-write implementation.
- Report includes commit, firmware hash, environment, APP/DATA sizes, chunk count, reconnect count, duration, verification result, and gaps.

## 5. Failure evidence and attribution

Stop subsequent destructive actions and preserve page logs, browser-console errors, UART logs if available, requested addresses/lengths, returned lengths, chunk indices, cumulative bytes, reconnect/connect/sync timestamps and errors, device inventory, commit, firmware SHA-256, commands and exit codes.

Initial attribution:
- Host test failure: chunk algorithm or test issue.
- Discontinuous offsets, short reads, or byte mismatch: read protocol, buffering, or address calculation.
- Reconnect/sync failure: transport lifecycle, stub/loader session, or serial recovery.
- Short reads pass but long reads still hang: sustained-read/session stability remains unresolved.
- Device reboot, panic, or partition-range anomaly: firmware/device issue; preserve state and stop.

Do not bypass verification, skip read-back, or fabricate reconnect evidence.

## 6. Report requirements

Include test time, branch and full commit SHA; firmware SHA-256; device/firmware type; OS/browser/Node; APP/DATA byte lengths; chunk count; 512 KiB boundaries and reconnect count; per-stage duration, errors/retries, byte-comparison and hash results; report/log paths; PASS/FAIL/BLOCKED with evidence-based attribution; and whether firmware 28 received human DATA confirmation.

Green CI is only an entry gate; it does not mean this real-device handoff has passed.
