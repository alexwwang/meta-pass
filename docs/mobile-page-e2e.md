# Mobile Viewport E2E for the Device Phone-Install Page

Status: implementation in progress on `feat/storage`.

## Goal and approach

Test the actual ESP32-hosted phone-install page using Playwright Chromium mobile emulation. The browser uses a mobile viewport, DPR, touch capability, and Android mobile User-Agent, but the page and API requests target the real device. Android Emulator, Appium, and a native host app are not prerequisites.

The runner follows the USB E2E principles in `tools/realdevice/usb_web_e2e_multi.mjs`: one browser session for the whole scenario, UI-only mutations, independent read-only device API verification, redacted reports and evidence, explicit hardware opt-in, and cleanup limited to uniquely named test slots.

## Scenario coverage

- Open the device page in a mobile viewport; verify module mount, mobile flags, and no document-level horizontal overflow.
- Capture the baseline slot list and assert the installer is idle.
- Search for two configured play IDs; open detail, choose slot/name, and confirm installation through the UI.
- Wait for the real page to report completion (including device confirmation/reconnect delays), then verify valid slots via read-only device APIs.
- Verify both plays coexist; delete each through the page's two-step Space Management UI; when archive data exists, select the page's erase-data option for test-owned data.
- Verify the second play survives the first deletion and compare final slots/data with the initial baseline.
- Save a JSON/text report, screenshot, page source, browser errors, and failed requests. On failure, cleanup attempts only slots whose names exactly match this run's generated names; ambiguous state is never deleted.

## Run

```bash
node tools/realdevice/mobile_page_e2e.mjs --real-device \
  --url 'http://<device-ip>/' --token '<32-hex-session>' \
  --play-a <id-a> --play-b <id-b>
```

Alternatively set `MOBILE_E2E_URL`, `META_PASS_SESSION`, `MOBILE_E2E_PLAY_A`, and `MOBILE_E2E_PLAY_B`. The token is added to the page URL fragment internally and redacted from artifacts. Node.js, the existing Playwright dependency, and a Chromium browser are required.

This is not an Android/WebView container test. Native WebView bridge, system permissions, and host-app lifecycle remain separate concerns. A normal cloud CI run has no physical device, so it runs syntax and static contract tests only; only a run against a real device can prove the business E2E passed.
