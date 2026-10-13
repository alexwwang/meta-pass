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

## 3. Impact assessment and minimum regression set

The runtime change is narrowly scoped to the USB installer page's **post-write Flash read-back verification**:
- New shared helper: `install-slot/flash-read-transfer.js` (bounded reads, byte comparison, progress, reconnect callback).
- Page integration: `install-slot/install-slot.html` uses it for APP read-back and DATA write/restore read-back.
- Serving and automated checks: `tools/install-slot/server.mjs`, `tools/install-slot/test-flash-read-transfer.mjs`, and the corresponding static contract test.

The existing erase/write loops, partition allocator, firmware/bootloader code, mobile embedded page, install/delete planning, and archive format are not changed by this optimization. Do not rerun unrelated mobile-page, allocator, boot, full install/delete lifecycle, or general backup-format scenarios solely for this change.

Run only these regression items:

### R1 — Host regression (required; no device writes)
Run `./tools/validate.sh --static` on the exact HEAD. It must exercise `test-flash-read-transfer.mjs` and the page integration contract. Confirm cases for contiguous offsets and bounded lengths, reconnect at 512 KiB, progress, short/invalid reads, byte mismatch, and empty payload. Firmware checks are an entry gate for using the built page/device, not evidence that Web Serial reconnect works.

### R2 — APP post-write read-back (required on device)
Using the USB service page, install an approved APP image larger than 512 KiB (preferably several MiB) so multiple boundaries are crossed. Verify that the existing write completes unchanged, then read-back verification completes using reads no larger than 32 KiB, reconnects/resyncs at each 512 KiB boundary, and byte comparison passes. Capture logs for at least two reconnect boundaries where image size allows.

### R3 — DATA post-write/read-back (required on device)
Using an existing safe test APP/DATA setup, exercise the DATA restore/write path that calls the shared verifier. Use a DATA payload larger than 512 KiB (preferably several MiB) to cross multiple boundaries. Verify write behavior remains unchanged and read-back completes with contiguous offsets and byte-for-byte equality. Do not create/delete unrelated slots just to broaden coverage.

### R4 — Recovery failure semantics (host tests required; device failure only if naturally encountered)
The host suite must prove short reads, invalid returned lengths, byte mismatch, and reconnect/sync errors fail closed. If reconnect or sync fails on device, capture the error and stop; do not retry by bypassing verification or mark the case PASS.

### Explicitly out of scope
Do not run full backup-archive creation/format compatibility, launcher upgrade, firmware boot, mobile WebView/browser E2E, generic partition-size selection, allocator, or APP deletion lifecycle regression for this read-back-only change. Existing backup creation and probe/metadata reads do not use this new helper. If DATA restoration is not safely available, report R3 as BLOCKED rather than substituting unrelated tests.

## 4. Pass criteria

- R1–R4 required host checks pass; latest Static checks and Firmware checks are green for the tested HEAD.
- APP and DATA device read-back each cross at least one 512 KiB boundary; preferably multiple boundaries with multi-MiB payloads.
- Logs prove every requested read is at most 32 KiB and every 512 KiB boundary triggers disconnect → connect → sync before the next read.
- Read offsets are contiguous; no short reads, gaps, duplicates, mismatch, unexplained timeout, or permanent hang.
- Existing write/erase behavior is unchanged; no claim is made that unrelated backup, mobile, or lifecycle paths were regression-tested.
- The report records exact HEAD, firmware SHA-256, payload sizes, chunks, reconnect count/timing, byte-comparison result, logs, and PASS/FAIL/BLOCKED per item.

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
