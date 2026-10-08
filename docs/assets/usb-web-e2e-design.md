# USB Web UI Real-Device E2E Test Design

Date: 2026-10-08  
Branch: `feat/storage`

## Purpose

Existing validation already covers:

- `tools/realdevice/smoke.py`: real-device HTTP/storage contract;
- `tools/realdevice/browser_smoke.mjs`: real `phone-install.js` × real device, including APP/DATA.

The remaining unverified layer is:

> **USB Web UI → Chromium Web Serial → esptool-js → ESP32-C3 → Flash**

This E2E test must exercise that chain through the actual USB page. It must not call `runInstall()` directly, mock Web Serial, or mock `/api/install/*`.

## Design

- Reuse the existing `install-slot/install-slot.html`, `server.mjs`, `mock-device.js`, and `window.__installSlotDebug`.
- Add only a minimal `?e2e=real` connection seam: instead of opening the OS chooser, the page selects exactly one previously-authorized real port from `navigator.serial.getPorts()`. The normal production path remains `requestPort()`.
- Use a thin Chromium browser runner. It is not a second installation implementation or protocol client.
- Real-device mode is explicit; normal/static CI never touches hardware.
- Use APP-only for this E2E. DATA remains covered by `browser_smoke.mjs` P2.
- Prefer Community Play so the page itself performs metadata fetch, firmware download, size validation, SHA-256 verification, and `extractAppImage()`.
- Capture baseline slots/status, perform the UI installation, independently verify slots/status through the device API, then remove only the test-created slot and verify exact restoration.

## Required stages

1. **E0 Environment** — Chromium, Web Serial, server, authorized port, ESP32-C3 and device API.
2. **E1 Baseline** — save `/api/install/slots` and `/api/install/status`.
3. **E2 Page** — open `http://localhost:4191/?e2e=real`; verify page/module readiness and no mock mode.
4. **E3 Connect** — click the real Connect button; require actual `navigator.serial → Transport → ESPLoader`.
5. **E4 Slots** — compare UI/debug slot model against device API baseline.
6. **E5 Play** — use Community Play → Fetch.
7. **E6 Install** — click Install and let the page perform download → SHA-256 → extraction → allocation → record/table → Flash write → metadata.
8. **E7 UI Done** — require successful UI status and expected log milestones.
9. **E8 Device Verify** — independently read `/api/install/status` and `/api/install/slots`; verify offset, size, imageLen and VALID state.
10. **E9 Flash/State** — require the new slot to persist in the real device model; do not duplicate existing byte-level browser-smoke coverage.
11. **E10 Cleanup** — prefer the UI Remove path; use device API only as recovery when the UI loses the loader.
12. **E11 Restore** — require post-cleanup state to equal the baseline apart from expected internal sequence changes.

## Cleanup safety

Every test-created slot is registered immediately. Deletion requires matching offset + size + test identity (play/name marker). Objects whose ownership cannot be proven are never deleted.

All paths use:

```
try {
  baseline();
  browserE2E();
} finally {
  cleanup();
  verifyBaseline();
}
```

Cleanup failure is a test failure.

## Evidence

Each run writes under:

`tools/realdevice/logs/usb-web-e2e-YYYYMMDD-HHMMSS/`

including metadata, baseline/final slots and status, browser console/errors, page log, screenshots, and JSON/Markdown reports.

The final report must explicitly state:

- USB Web UI → Web Serial → ESP32-C3 → Flash: PASS/FAIL
- direct `phone-install.js`: NOT USED
- mocked Web Serial: NOT USED
- mocked `/api/install/*`: NOT USED
- DATA: covered by existing browser smoke
- physical power loss: NOT TESTED

## Review conclusion

The design is approved for implementation because it adds only the missing outer integration layer, preserves existing test boundaries, uses real browser/device paths, is opt-in for hardware, and provides baseline-safe cleanup plus independent device-side verification.
