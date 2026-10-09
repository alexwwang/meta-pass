<p align="right">
  <a href="mobile-embedded-page-e2e.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Mobile Embedded-Page E2E Framework

## Purpose

This runner drives the phone-side page through an actual Android automation session. It is separate from `usb_web_e2e_multi.mjs`: the latter tests the desktop USB/Web Serial page and must not be treated as evidence for the phone UI.

## Architecture

- Appium + UiAutomator2 owns the Android device and launches either Chrome or the configured native host app.
- The runner discovers available contexts and switches into a real `WEBVIEW_*` / `CHROMIUM` context. It does not emulate a phone by shrinking desktop Chrome.
- UI interactions are defined in a JSON scenario (`waitVisible`, `click`, `fill`, `waitText`, `assertText`, `assertJs`, `sleep`).
- The UI is the only write path. A read-only `GET /api/install/slots` check can independently verify device state; direct install/remove APIs are forbidden.
- Failure evidence includes a redacted page source, screenshot, JSON report and readable report. Tokens and IPs are redacted from text artifacts.

## Prerequisites

1. Android device with USB debugging enabled and authorized by ADB.
2. Appium 2 server reachable at `http://127.0.0.1:4723`, with `uiautomator2` driver installed.
3. For `--mode webview`, supply the package and launch activity of the app that hosts the embedded page. The app must expose a debuggable WebView.
4. The phone and ESP32 must share a reachable network. Use the device URL displayed by the firmware; when token-based access is used, pass the complete URL including its fragment via environment/CLI. Do not put secrets into committed scenario files.
5. Node.js supported by the repository's E2E tools. The runner uses Node built-ins and adds no npm dependency.

## Run

From the repository root:

```bash
# Native app with embedded WebView (the target mode)
node tools/realdevice/mobile_webview_e2e.mjs \
  --mode webview --app-package <package.id> --app-activity <activity> \
  --url 'http://<device-ip>/' --ip <device-ip> --scenario tools/realdevice/mobile-webview-scenario.json

# Android Chrome mobile-browser path (a separate mode, not a substitute for WebView)
node tools/realdevice/mobile_webview_e2e.mjs \
  --mode browser --url 'http://<device-ip>/' --ip <device-ip>

# Override the Appium server / test timeout
APPIUM_URL=http://127.0.0.1:4723 MOBILE_E2E_TIMEOUT_MS=45000 \
  node tools/realdevice/mobile_webview_e2e.mjs --mode webview \
  --app-package <package.id> --app-activity <activity> --url 'http://<device-ip>/'
```

The URL fragment is not sent in the HTTP request, but can still appear in shell history. Avoid copying it into committed scenario files or logs.

## Scenario format

The committed scenario is intentionally a connectivity/shell smoke test: it verifies a non-empty mobile document and a readable slot list without inventing selectors for an app UI that is not part of this repository. Extend a local scenario with selectors confirmed from the real host app to cover the full product journey.

Supported actions:

- `waitVisible`: `selector`, optional `timeout`
- `click`: `selector`
- `fill`: `selector`, `value` or `valueFrom` (`deviceIp`, `deviceUrl`)
- `waitText` / `assertText`: `selector`, `text`
- `assertJs`: `script`, optional `equals`
- `snapshotSlots`: `snapshot` name; records a device-state snapshot at that point in the UI flow
- `assertSlotPresent` / `assertSlotAbsent`: `snapshot` plus `slotName` or `playId`
- `assertInstallerIdle`: assert the phone install service has no active session
- `sleep`: `ms`

Example step:

```json
{ "action": "waitText", "name": "install completes", "selector": "#install-status", "text": "done", "timeout": 240000 }
```

The selectors above are examples only; use selectors from the actual embedded page, not the desktop USB installer.

## Current coverage and honest boundary

- Framework capabilities: Appium session lifecycle, Android app/browser launch, real WebView discovery/switching, scenario-driven UI control, device-state readback, redacted artifacts, deterministic exit status and session cleanup.
- Default scenario: mobile page/shell smoke only.
- Full install → progress → completed → remove → baseline restoration requires a scenario whose selectors match the deployed native host page. The host application package/activity and exact page DOM are deployment-specific and are intentionally not guessed in this repository.
- No physical phone/Appium device is attached to cloud CI, so device E2E is not run by static CI. Host-side syntax and contract checks are the CI gate.

## Evidence

Artifacts are written to `tools/realdevice/logs/mobile-e2e-<timestamp>/`: `report.json`, `report.txt`, `device-slots.json`, and on failure `page-source.xml` / `failure.png`.
