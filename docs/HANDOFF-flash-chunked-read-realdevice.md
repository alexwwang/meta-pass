# Handoff | feat/storage Flash Chunked-Read Real-Device Validation

Date: 2026-10-13  
Target branch: `feat/storage`  
Test the latest `origin/feat/storage` HEAD at execution time. Do not reuse PASS results from an older commit.

Language: [简体中文](HANDOFF-flash-chunked-read-realdevice.zh_CN.md)

## 1. Goal

Verify that long Flash reads remain reliable when split into bounded chunks with periodic serial-session recovery. The target is **Flash reading and read-back verification**, not chunked writing.

Expected implementation:
- The existing backup-restore APP/DATA post-write verification uses the ESP stub's `flashMd5sum(address, length)` to hash the complete target range on-device, then strictly compares it with the archive payload MD5.
- Each non-empty APP/DATA range requires one device-side digest command; the page no longer transfers the entire range back through Web Serial, avoiding the reported deterministic wedge after 30 `readFlash` calls per page/SerialPort session.
- Malformed device digests, digest mismatches, unavailable local MD5, or serial-command errors must fail closed; restore must not commit.
- `flash-read-transfer.js` remains as an independent chunked byte-comparison tool for supplemental device evidence; it is no longer the page's production post-write verifier.
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

The runtime change is narrowly scoped to the USB installer page's **backup-restore post-write Flash read-back verification**:
- New production verifier: `install-slot/flash-readback-verify.js`, comparing a device-side Flash MD5 with the local payload MD5 for the complete target range.
- Page integration: `install-slot/install-slot.html` uses digest verification for APP restore read-back and DATA restore write/read-back; the ordinary local/community APP install flow is not an existing read-back call site in this change.
- `install-slot/flash-read-transfer.js` and its tests remain for independent chunked byte-by-byte device validation; the page's production path no longer performs long read-back streams.
- Serving and automated checks: `tools/install-slot/server.mjs`, `tools/install-slot/test-flash-readback-verify.mjs`, and the page contract test.

The existing erase/write loops, partition allocator, firmware/bootloader code, mobile embedded page, install/delete planning, and archive format are not changed by this optimization. Do not rerun unrelated mobile-page, allocator, boot, full install/delete lifecycle, or general backup-format scenarios solely for this change.

Run only these regression items:

### R1 — Host regression (required; no device writes)
Run `./tools/validate.sh --static` on the exact HEAD. Confirm it runs `test-flash-readback-verify.mjs`, `test-flash-read-transfer.mjs`, and the page integration contract. Digest tests must cover one device digest request for the full range, matching/mismatching digests, malformed responses, local MD5 errors, serial-command failures, and rejection of empty payloads; the chunked utility still covers offsets, bounded chunks, reconnects, short reads, and byte mismatches. Firmware checks remain a gate for device testing.

### R2 — APP post-write read-back (required on device)
Using the USB page's existing backup-restore flow, restore an approved APP image larger than 512 KiB (preferably several MiB). Confirm the existing write completes unchanged, then verify the complete range with the device-side `flashMd5sum` command. Logs must include address, length, expected MD5, device MD5, and success, with matching digests; this path should not issue a bulk `readFlash` stream. Do not count ordinary local/community APP installation as this regression: it is not an existing read-back verifier call site.

### R3 — DATA post-write/read-back (required on device)
Using an existing safe test APP/DATA setup, exercise the DATA restore/write path with a payload larger than 512 KiB (preferably several MiB). Confirm write behavior is unchanged and the device computes an MD5 for the complete DATA range that matches the local payload digest; the transaction must commit only after this succeeds. Do not create/delete unrelated slots.

### R4 — Recovery failure semantics (host tests required; device failure only if naturally encountered)
The host suite must prove digest mismatch, malformed device digest, local MD5 failure, and device digest/serial-command errors fail closed. If device verification fails, preserve evidence and confirm the restore transaction did not commit and staged DATA followed the existing cleanup path; never bypass verification.

### Explicitly out of scope
Do not run full backup-archive creation/format compatibility, launcher upgrade, firmware boot, mobile WebView/browser E2E, generic partition-size selection, allocator, or APP deletion lifecycle regression for this read-back-only change. Existing backup creation and probe/metadata reads do not use this new helper. If DATA restoration is not safely available, report R3 as BLOCKED rather than substituting unrelated tests.

## 4. Pass criteria

- R1–R4 required host checks pass; latest Static checks and Firmware checks are green for the tested HEAD.
- APP and DATA post-write verification both pass using device-side MD5 over the complete ranges, with matching expected/device digests in logs.
- The production path is not expected to emit 32 KiB read chunks or 512 KiB reconnect logs; it should avoid long Web Serial read-back streams.
- No digest mismatch, malformed response, unhandled command error, unexplained timeout, or permanent hang; failures remain fail-closed.
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

Include test time, branch and full commit SHA; firmware SHA-256; device/firmware type; OS/browser/Node; APP/DATA byte lengths; device-side digest request count and target ranges; expected/device MD5 values; per-stage duration, errors/retries; report/log paths; PASS/FAIL/BLOCKED with evidence-based attribution; and whether firmware 28 received human DATA confirmation.

Green CI is only an entry gate; it does not mean this real-device handoff has passed.
