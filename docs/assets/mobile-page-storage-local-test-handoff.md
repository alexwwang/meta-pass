# Handoff: feat/storage Local Validation and Real-Device Testing

[简体中文](mobile-page-storage-local-test-handoff.zh_CN.md) | English

Date: 2026-10-11  
Repository: `alexwwang/meta-pass`  
Branch: `feat/storage`  
Handoff baseline: `8259c37dbe70bf2fef3593ca750ac9cdc09c8033`  
PR: [#1](https://github.com/alexwwang/meta-pass/pull/1)

## 1. Goal and Current State

The local agent's goal is to complete validation with the existing automated test framework and, once prerequisites are in place, run a controlled real-device test to verify dynamic DATA partition management and the APP/DATA lifecycle.

**Do not build a separate USB or mobile-page test framework.** The repository already has USB Web E2E, mobile-page viewport smoke, mobile WebView E2E, and storage-management E2E. Reuse and extend these tests.

Known state at handoff:

- GitHub Actions Static checks: passing at commit `8259c37d`.
- GitHub Actions Firmware checks: passing at commit `8259c37d`.
- These two are not local real-device E2E evidence.
- There is not yet evidence that the storage-management E2E has passed on a real device this round.
- Do not merge the PR and do not develop directly on `main`.

## 2. Sync and Inspect the Workspace First

Run from the repository root:

```bash
git fetch origin
git switch feat/storage
git status --short --branch
git log -1 --oneline
```

Confirm the branch tracks the latest remote `feat/storage` and check for local uncommitted changes. Do not overwrite or discard the user's local changes. If the remote has moved ahead, review the diff before fast-forwarding/syncing; do not force-push and do not reset the user's workspace.

## 3. Existing Test Entry Points to Reuse

| Purpose | Existing entry | Notes |
|---|---|---|
| Repo static checks, contract tests, script syntax checks | `./tools/validate.sh --static` | Run first; if it fails, fix before any destructive real-device test |
| Mobile-page multi-viewport, non-destructive smoke | `tools/realdevice/mobile_page_browser_smoke.mjs` | Uses existing Chromium CDP; no write clicks, no write API calls |
| Mobile-page storage-management end-to-end | `tools/realdevice/mobile_page_e2e.mjs` | Main real-device path for this DATA/APP lifecycle; installs, boots, and uninstalls test APPs and writes test DATA |
| Mobile browser/WebView scenario | `tools/realdevice/mobile_webview_e2e.mjs` | Reuses existing scenario file `tools/realdevice/mobile-webview-scenario.json`; run only when target entry/host WebView coverage is needed |
| USB web install flow | `tools/realdevice/usb_web_e2e.mjs` | Reuses existing Web Serial flow; operates on the real device |
| USB multi-round install/uninstall and reboot persistence | `tools/realdevice/usb_web_e2e_multi.mjs` | Suitable for USB install, coexistence, deletion, and post-reboot persistence; modifies the device |

Related existing contract tests invoked by `tools/validate.sh --static`:

- `tests/test_usb_web_e2e_contract.py`
- `tests/test_mobile_webview_e2e_contract.py`
- `tests/test_mobile_page_e2e_contract.py`
- `tests/test_mobile_page_browser_smoke_contract.py`
- `tests/test_mobile_page_storage_management_contract.py`

## 4. Execution Order

### Phase A: Local Non-Destructive Checks

1. Check Node.js, Python versions, and workspace state.
2. Run `./tools/validate.sh --static`.
3. Confirm new/modified automation scripts pass syntax and contract checks.
4. Start the test page server and run the mobile-page smoke with local Chromium/Chrome CDP.

Example (adjust port and URL to the actual local server):

```bash
node tools/realdevice/mobile_page_browser_smoke.mjs \
  --url 'http://127.0.0.1:4191/?mock=1' \
  --cdp-url 'http://127.0.0.1:9222'
```

If the local server needs other startup parameters, check the project's existing startup scripts and docs first; do not invent a new serving method. The smoke covers viewports including 320×720, 360×800, 390×844, and 430×932 by default. Check the report for key controls, horizontal overflow, page errors, console errors, and failed requests.

### Phase B: Real-Device Environment Preparation

Before any write operation, confirm each item:

- Use a dedicated test device with no user data to preserve.
- The device runs launcher firmware built with `CONFIG_META_E2E_TEST_CONTROL=y`.
- The management page is reachable, the installer is idle, and the baseline slot table and DATA reservation are valid.
- Two distinct digital play IDs dedicated to testing are prepared; their firmware supports the DATA test protocol.
- Node.js (22+ recommended), Python 3, and `pyserial` are installed.
- Local Chromium/Chrome CDP listens on loopback only, not exposed to the LAN.
- The correct launcher USB Serial/JTAG port is identified.
- The session token is supplied interactively via environment variable — never in command-line arguments, logs, or commits.
- Report locations and failure-recovery rules have been read.

Environment preparation example:

```bash
python3 -m pip install pyserial

# Example; adjust browser executable and profile path for your machine.
google-chrome --remote-debugging-port=9222 \
  --user-data-dir=/tmp/meta-pass-cdp

export MOBILE_E2E_URL='http://<device-host>/'
read -rsp 'Session token (32 hex): ' META_PASS_SESSION; echo
export META_PASS_SESSION
export MOBILE_E2E_CDP_URL='http://127.0.0.1:9222'
export META_PASS_E2E_SERIAL_PORT='/dev/<launcher-usb-serial-device>'
```

Do not paste real tokens, device IPs, full URLs, or sensitive host paths into PRs, issues, or shared logs.

### Phase C: Run the Mobile-Page Storage-Management E2E

Only after Phases A and B pass, run from the repository root:

```bash
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device \
  --runtime-driver tools/realdevice/mobile_page_runtime_driver.py \
  --require-data-reservation \
  --play-a <dedicated-test-play-id-a> \
  --play-b <dedicated-test-play-id-b>
```

Both play IDs must be explicitly provided by the executor, distinct from each other; do not use default IDs and do not pick real apps containing user data. This test really installs/uninstalls test APPs and writes test DATA — it is not a read-only smoke.

Verify the following paths and independent postconditions:

1. Installer baseline idle; dynamic slot geometry, alignment, overlap, and free-space calculations correct.
2. APP A installs successfully; device API independently reads back its VALID slot.
3. Canceling a delete does not change the slot table, DATA reservation, or installer state.
4. The child firmware actually boots; DATA erase, write, read-back, checksum, and reboot persistence have serial evidence.
5. APP B coexists with A after install; their APP slots and DATA physical extents do not overlap.
6. After deleting A, A's APP/DATA resources are released and B remains valid and runnable.
7. Reinstall under A's play ID with a new random name C; confirm C gets an independent slot and new DATA reservation; after uninstalling C, confirm all new resources are released.
8. After deleting B, B's APP/DATA resources are released and device state returns to the pre-test baseline.
9. Regardless of pass or fail, independently read the final slot table, DATA reservations, installer status, and dynamic partition geometry.

The report defaults to:

`tools/realdevice/logs/mobile-page-e2e-<timestamp>/report.json`

The final success condition is `verdict=PASS` in the report plus all device-run evidence and postconditions holding. A UI success message, passing static contract tests, or a successful firmware build alone do not count as a real-device E2E pass.

### Phase D: Reuse USB E2E When Necessary

If this round's changes affect the USB install page, the shared installer protocol, or common APP install/delete behavior, continue with the existing USB E2E; do not duplicate code into a new harness.

Before running, read the script's current usage and safety gates. Note especially: the default play IDs in the USB scripts are not suitable as real test targets — pass dedicated test IDs explicitly, and confirm target device, IP, session, and serial port. Run only on a dedicated device. The USB multi-round script modifies the device and performs reboot/persistence checks.

If this round only verifies mobile-page-specific DATA reservation/partition-management behavior, complete Phase C first; do not re-flash or wipe the device aimlessly "for more coverage".

## 5. Failure and Device-Recovery Rules

- If any safety gate fails, stop further destructive device operations.
- If the installer does not return to idle after an install, do not continue with further device writes.
- If A/B DATA physical isolation cannot be verified, stop further writes and run the safe cleanup/recovery flow.
- After a failure, clean up only the APPs created under this round's random names; do not delete APPs that existed in the pre-test baseline.
- Check `deviceRecovery.status` in the report:
  - `RESTORED`: final device state was independently read back and matches the baseline.
  - `NOT_RESTORED`: device not restored was detected.
  - `UNKNOWN`: recovery state cannot be reliably confirmed.
- Only `RESTORED` justifies claiming verified recovery. If `NOT_RESTORED` or `UNKNOWN`, stop rerunning and manually inspect the device and recovery snapshot first.
- Keep sanitized reports, failure phase, key serial evidence, and reproduction steps. Do not skip assertions, lower verification standards, or modify test results to obtain PASS.

## 6. Delivery Report

After completion, report back to the project owner:

1. Commands actually executed and environment (no secrets/tokens).
2. PASS/FAIL/SKIP for each test, with reasons for any not run.
3. Report directory and shareable sanitized reports/evidence.
4. Whether DATA real write, read-back, checksum, and reboot persistence were verified.
5. Final `deviceRecovery.status` and baseline comparison result.
6. Issues found, files involved, fix commits.
7. Latest branch SHA and CI status.

Do not mark unrun tests as PASS, do not call a simulated page smoke a real-device verification, and do not merge the PR; all fixes continue on the `feat/storage` branch.
