# Handoff | feat/storage Real-Device Backup and Restore Acceptance

Language: [简体中文](HANDOFF-realdevice-backup-restore-manual-data-confirmation.zh_CN.md)

Date: 2026-10-12  
Target branch: `feat/storage`  
Scope: APP + DATA backup/restore in the embedded mobile page and USB service page, including real-device DATA access confirmation.

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
- Recent changes introduced shared v2 archive preparation in `install-slot/data-archive-restore.js`, used by both mobile and USB restore paths, with host test `tools/install-slot/test-data-archive-restore.mjs`.
- A recent static check failed because the Chinese handoff did not have its required English default peer. Fix this documentation contract and rerun checks on the latest HEAD; do not reuse results from an older commit.
- Confirm current branch, commit, working tree, host tests, and firmware build before changing a device.

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

- `tools/install-slot/test-data-byte-archive.mjs`
- `tools/install-slot/test-data-archive-restore.mjs`
- `tools/install-slot/test-data-archive-zip.mjs`
- `tools/install-slot/test-data-restore-transaction.mjs`
- `tools/install-slot/test-data-restore-device-integration.mjs`
- `tests/test_phone_install.mjs`
- Mobile and USB storage-management/page-health contract tests.

Also verify:

- Mobile and USB use the same `prepareDataArchiveRestore()` semantics for v2 APP+DATA archives.
- v2 restore validates archived NVS bytes but does not write global NVS to the target device.
- If commit status is uncertain, the restore transaction does not delete possibly referenced staging DATA.
- DATA length, SHA-256, and allocation granularity are validated.
- The current commit's firmware build succeeds.

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
3. Confirm the archive contains only the selected APP group and its DATA; record whether global NVS is included.
4. NVS archives may contain Wi-Fi credentials and device configuration and are unencrypted. Do not upload/share them or print raw NVS bytes in logs.
5. Store the archive securely and record its size and SHA-256. Never commit the archive to Git.

### T4 — Restore to a safe target

1. Confirm the target device is approved for writes and the archive source is trusted.
2. Record target APP/DATA/NVS baseline.
3. v2 restore must restore APP + DATA only. Validate archived global NVS but do not write it to the target.
4. Record preflight, APP/DATA write/read-back, carve commit, and final inventory.
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
2. Both embedded mobile and USB backup/restore paths have real-device test records.
3. Every installation involving real-market firmware 28 has human DATA access confirmation; unconfirmed scenarios remain BLOCKED.
4. Restored APP/DATA can actually be read and written, and values remain correct after required reread/restart checks.
5. Archive structure, byte lengths, and SHA-256 pass, with evidence for restored DATA and target device state.
