# Handoff | feat/storage Real-Device Backup and Restore Acceptance

Language: [简体中文](HANDOFF-realdevice-backup-restore-manual-data-confirmation.zh_CN.md)

Date: 2026-10-14  
Target branch: `feat/storage`  
Scope: v2 whole-device storage snapshots and human confirmation of real-device DATA access after installs. The unchanged USB v1.1 compatibility branch is outside this round's test scope.

## 0. Mandatory test discipline

**The real-market firmware known as “firmware 28” cannot participate in the current automation flow. After every successful installation involving it, stop and wait for a human to confirm that DATA reads and writes work.**

The local agent must follow these rules:

1. Immediately pause after installation succeeds. Do not start another install, delete, restore, or cleanup action.
2. Ask the user/on-site tester to check DATA access and wait for an explicit response. Do not auto-release the gate after a timeout or assume success.
3. **Never skip this human-confirmation task to finish the test run or make the report green.** HTTP 200, installation success, a launchable APP, clean logs, mocks, or simulated data are not substitutes.
4. Until explicit confirmation arrives, mark the scenario `BLOCKED_WAITING_FOR_HUMAN_DATA_CONFIRMATION`. It is not PASS, and dependent destructive steps must not proceed.
5. If the execution environment cannot wait for human input, stop at the gate, preserve evidence, and report that confirmation is pending.
6. Record who/what confirmed, when, firmware and play identifiers, DATA actions performed, result, and notes. Missing, ambiguous, or unavailable confirmation is not PASS.
7. Do not claim automation coverage for firmware 28. Report its manual acceptance separately.

These rules override the runner's default behavior and any goal of completing all tests in one pass.

## 1. Current status and entry conditions

- Continue on `feat/storage`; do not develop on `main`.
- v2 archives now mean whole-device allocated-storage snapshots, replacing the APP+DATA logical archive. The discriminator is `kind: meta-pass-storage-snapshot`.
- The mobile page supports snapshot export and restore preflight, but not restore writes. The device currently exposes guarded partition inventory/read endpoints only. The USB v1.1 compatibility branch is unchanged and requires no dedicated regression in this round.
- USB fixed-slot devices retain historical v1.1 archive behavior. USB dynamic-v2 snapshot backup is blocked because the serial transport cannot yet reliably identify the running host partition. Neither page may treat an old APP+DATA v2 archive as a new snapshot.
- Static checks and firmware checks passed for the most recently verified commit `3900829f91d15ffffe1341b50960e2c4df1f2d7a`. Recheck CI on any newer HEAD; never reuse an older result.

Suggested commands:

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
git status --short
git rev-parse HEAD
./tools/validate.sh
```

Activate ESP-IDF 5.5.3 before firmware builds. Record real exit codes and logs. Never transfer a PASS from an older commit to the current HEAD.

## 2. Separate test firmware from market firmware

Keep separate records for:

- **Automation-capable development/test firmware:** API behavior, transport interruption, resume offsets, archive validation, partition records, and repeatability.
- **Real-market firmware 28:** current automation does not cover it; every successful installation must stop for human DATA read/write confirmation.

For each scenario, record the device identifier (redacted in reports if needed), firmware type/version/hash, repository commit, page entry point, operation, APP/play ID, DATA label/subtype and expected bytes, logs, and human confirmation status. Do not replace or falsify market-firmware behavior for automation.

## 3. Required code-level validation

Before device testing, run:

```bash
./tools/validate.sh
```

Confirm that these tests actually execute and pass:

- `tests/test_storage_snapshot.mjs`
- `tests/test_storage_snapshot_device_contract.py`
- `tools/install-slot/test-data-restore-device-integration.mjs` (now tests snapshot export and non-mutating preflight, not APP+DATA writes)
- `tests/test_phone_install.mjs`
- `tests/test_mobile_page_storage_management_contract.py`
- USB storage-management and browser smoke contract tests.

Also verify:

- Snapshot completeness, payload length, and SHA-256 checks pass.
- Target layout must match exactly; running host firmware and `cardid` are excluded.
- Mobile restore preflight only reads partition inventory and does not write Flash.
- USB dynamic v2 snapshot remains blocked. Do not spend this round re-testing the unchanged USB v1.1 historical branch.
- Static checks and firmware checks pass on the current commit; real-device tests remain separate.

If any check fails, fix it and rerun affected tests before proceeding to device acceptance.

## 4. Real-device test sequence

### T0 — Record baseline and safety state

1. Record device, firmware type, commit, page, and current APP/DATA inventory.
2. Confirm communication, pairing, and page version.
3. Before deletion or overwrite, verify a usable backup exists and that the device/firmware is eligible for this test.
4. If device identity, firmware version, or baseline inventory is unclear, stop before destructive operations.

### T1 — Install APP + DATA on development/test firmware

1. Install a test play with identifiable initial DATA through the target page.
2. Verify API state, APP/DATA inventory, allocation ranges, and final state.
3. If safe automated DATA reads are available, save initial bytes, offset, length, and SHA-256, and compare byte-for-byte.
4. Then launch the play on the device and perform actual DATA read/write actions.
5. Record reboot, errors, and any DATA initialization behavior.

### T2 — Mandatory human confirmation after every successful installation

This gate is mandatory, especially for market firmware 28:

1. Pause the runner/agent. Do not start another install, delete, restore, or cleanup.
2. Ask the user/on-site tester:
   - Does the play launch normally?
   - Can it read the known initial DATA state?
   - Can it write an observable value and read it back correctly, including after restarting the play if needed?
   - Were there data loss, stale/cross-app data, unexpected initialization, or I/O errors?
3. Wait for a human response. No response must never auto-pass.
4. Record the outcome:
   - Explicit confirmation of correct I/O: `PASS_HUMAN_CONFIRMED`
   - Failure: `FAIL_HUMAN_DATA_IO`; stop destructive tests and preserve state/evidence.
   - No reply or inconclusive feedback: `BLOCKED_WAITING_FOR_HUMAN_DATA_CONFIRMATION`.
5. Continue only after explicit human confirmation passes.

**Do not** substitute installation API success, APP launch, clean logs, HTTP status, host tests, emulators, fake devices, isolated Flash reads, or assumptions for human confirmation.

### T3 — Backup

1. Export an archive through an explicit user action.
2. Verify ZIP parsing, manifest completeness, each entry's byte length, and SHA-256.
3. For a mobile v2 snapshot, confirm the archive contains raw bytes of allocated partitions except the running host firmware, `cardid`, and unallocated gaps; NVS may be included.
4. ZIP files are unencrypted and may contain Wi-Fi credentials, install-session state, and play data. Do not upload/share them or print raw sensitive bytes. USB dynamic-v2 snapshot backup is currently blocked and must not fall back to a retired APP+DATA v2 archive.
5. Store the archive securely and record its size and SHA-256. Never commit the archive to Git.

### T4 — Restore to a safe target

1. Confirm the target device is approved for writes and the archive source is trusted.
2. Record target APP/DATA/NVS baseline.
3. Currently only archive validation and target-layout preflight are allowed; do not perform snapshot restore writes until transactional write/rollback exists.
4. Record target partition inventory, exact-layout result, payload hash validation, and the explicit no-write status.
5. After successful restore, repeat T2 human DATA confirmation. Do not continue without it.
6. If commit outcome is uncertain, stop. Do not blindly retry or clean staging; inspect device state and collect logs first.

### T5 — Delete and reinstall

1. Before deletion, verify a backup of the APP and all associated DATA exists and validates.
2. Delete the APP through the page; do not try to delete DATA separately.
3. Verify the APP record and its associated DATA are handled according to the design, and unrelated APP/DATA remain unchanged.
4. On reinstall, verify fresh-allocation behavior and ensure no other installed APP is overwritten.
5. After successful reinstall, stop at T2 and wait for human DATA confirmation again.

## 5. Failure and stop rules

Immediately stop further destructive actions if any of these occur:

- Human DATA confirmation fails or is inconclusive.
- Device identity or firmware differs from the test plan.
- Archive, DATA length/hash, Flash read-back, or carve state mismatches.
- Restore commit/finalize status is uncertain.
- APP deletion leaves DATA ownership or storage ranges unexplained.
- Device panic, repeated reboot, authentication errors, or suspicious partition-range behavior occurs.

Preserve device state and APP/DATA inventory, UART/page/command logs, HTTP status and response summaries (redact tokens), reports, archive SHA-256, DATA comparison, original human feedback and timestamp, firmware/commit/page, and stage identifier. Do not auto-clean the scene to keep tests running; preserve staging when commit status is uncertain.

## 6. Per-scenario report

Record each scenario separately; never merge results across firmware or page types.

| Field | Value |
|---|---|
| Scenario ID | T0/T1/T2/T3/T4/T5 + sequence |
| Time | Start/end |
| Device/firmware | Development/test or real-market firmware 28 |
| Repository commit | Full SHA |
| Page | Embedded mobile / USB service |
| Operation | Install / backup / restore / delete |
| APP/play ID | Actual value |
| DATA check | Read content, written value, read-back, post-restart result |
| Automation status | PASS / FAIL / NOT_RUN |
| Human status | PASS_HUMAN_CONFIRMED / FAIL_HUMAN_DATA_IO / BLOCKED_WAITING_FOR_HUMAN_DATA_CONFIRMATION / NOT_REQUIRED |
| Human feedback | Exact text or accurate summary and time |
| Evidence | Report, UART, HTTP summary, hashes, screenshots/photos |
| Attribution | Code / firmware / device environment / procedure / unknown |
| Next step | Fix, retest, or wait for human |

## 7. Definition of done

The task is complete only when all conditions hold:

1. Static/host tests and firmware build pass on the current HEAD.
2. Mobile snapshot backup/preflight and USB v1.1 legacy compatibility have test records; USB v2 snapshot and snapshot restore on both pages must remain explicitly BLOCKED until implemented.
3. Every installation involving real-market firmware 28 has human DATA access confirmation; unconfirmed scenarios remain BLOCKED.
4. Once snapshot restore is implemented, restored partitions must pass read-back verification; human DATA checks after any play install remain governed by T2.
5. Archive structure, byte lengths, and SHA-256 pass; preflight must never be presented as evidence of a completed restore.
