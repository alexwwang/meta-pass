<p align="right">
  <a href="storage-test-report-20261011.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Storage branch real-device test report — feat/storage handoff run

**Date**: 2026-10-11
**Branch**: `feat/storage`
**Final commit**: `f708a2f` (tree identical to the tested firmware; the flashed app was built at `60fc73c`, the later two commits touch docs only)
**Firmware**: local ESP-IDF 5.5.3 build with `CONFIG_META_E2E_TEST_CONTROL=y` overlay (`sdkconfig.defaults` + `sdkconfig.e2e-test.defaults`)
**Firmware SHA-256**: `3e7ae68a4b138b59ed2d465eac46651f76c3fc6008a65c93c50748d945e05f89` (1,133,184 bytes, app image at 0x10000; on-device version string `v2.1.0-668-g115efd9-dirty`)
**Device**: dedicated test unit, 192.168.0.24 (STA), serial `/dev/tty.usbmodem142401`, no user data to preserve
**Handoff**: [mobile-page-storage-local-test-handoff](assets/mobile-page-storage-local-test-handoff.md), baseline `8259c37`
**Raw evidence**: `tools/realdevice/logs/mobile-page-e2e-2026-10-11T04-21-15-137Z/` (best run; earlier runs' logs in sibling timestamped directories)

> ⚠️ The device currently runs the **test-control firmware**. Do not treat it as a release image.

---

## 1. Verdict

**FAIL — with device recovery RESTORED.** The only failing assertion chain is the child-runtime serial protocol (M04A/M05B): marketplace firmware does not speak the `e2edata` test protocol, so runtime DATA evidence is unreachable with market plays and case B is correctly blocked by the safety gates. Everything reachable with marketplace plays passed on hardware, including the first-ever successful phone-page install of Play 28 with a 3 MiB DATA carve.

Do not read this as a product regression: no production default behavior changed; the failing parts are the fixture-only runtime evidence and the capacity ceiling documented in §5.

## 2. Executed environment (no secrets)

```bash
./tools/validate.sh --static            # PASS
node tools/realdevice/mobile_page_browser_smoke.mjs \
  --url 'http://127.0.0.1:4191/m' --cdp-url 'http://127.0.0.1:9222'   # PASS 15/15

# Session token negotiated unattended via the test-only serial channel:
#   E2E SESSION_RESET  ->  "pair ready: code=… token=…" on UART

META_PASS_E2E_SERIAL_PORT=/dev/tty.usbmodem142401 \
META_PASS_SESSION=<token from UART> \
node tools/realdevice/mobile_page_e2e.mjs --real-device \
  --url 'http://192.168.0.24/?mp_test_data_profile=play28-recordings-3m#s=<token>' \
  --runtime-driver tools/realdevice/mobile_page_runtime_driver.py \
  --require-data-reservation --play-a 28 --play-b 582
```

## 3. Results by case

| Case | Result | Evidence |
|---|---|---|
| Static validation (`validate.sh --static`) | **PASS** | incl. new contract tests |
| Mobile viewport smoke (local `/m` shell) | **PASS 15/15** | 4 viewports, no overflow/console errors |
| M01 page health/viewports | **PASS** | on the real device page |
| M02 baseline safety gate | **PASS** | idle, geometry, free=6,766,592 |
| M03 search + detail (Play 28) | **PASS** | playId=28 |
| M04 install A via UI + device commit | **PASS** | UI complete; slot VALID; free 6,766,592→2,158,592 |
| M04 DATA reservation created for A | **PASS** | newReservations=1 (`recordings`, 3 MiB profile) |
| Cancel-uninstall no-op | **PASS** | slots unchanged, installer idle |
| M04A runtime A (serial `e2edata` protocol) | **FAIL (expected)** | market firmware never answers READY; timeout |
| M05 install B / M05B / M06 / M06C / M07 | **BLOCKED** | safety gates correctly refuse to continue |
| M09 page health at end | **PASS** | no console/page errors |
| Failure cleanup + restoration readback | **PASS** | `deviceRecovery=RESTORED`, 0 slots / 0 DATA / free restored |

**DATA write/readback/checksum/reboot persistence: NOT COVERED** — unreachable with marketplace firmware; only the `e2edata` fixture speaks the runtime protocol.

**USB E2E (handoff Phase D): SKIPPED** — this round only exercised the phone-page DATA path; the handoff forbids aimless re-flashing. The shared installer endpoints (prepare/session/chunk/finalize/remove) were all exercised by the mobile run above.

## 4. Fixes landed during this run (all on `feat/storage`)

| Commit | Fix |
|---|---|
| `7b9eadc` | English handoff peer + `node_modules/` gitignore (validate failure) |
| `115efd9` | Local dev phone shell `GET /m` (the documented smoke URL could never render `#mp-install-root`) + `/author-coffee.jpg` whitelist |
| `b33a571` | Runner idle gate: `active` is the store-open flag, never false with a live token session; `E2E SESSION_RESET` serial command; device `/favicon.ico` 204; `max_uri_handlers` 12→16 (13th registration failure killed the whole install service) |
| `60fc73c` | Runner drives the DATA capacity picker (`#mp-data-confirm`); `play28-recordings-3m` profile (worker + local server + tests); offer auto-confirm in the test control task |
| `ae94be8` / `f708a2f` | This report + CHANGELOG/docs updates |

## 5. Hardware-proven findings

1. **Pool segment ceiling.** The fixed `cardid` (0x356000) / `store` (0x35A000) entries split the 6.44 MiB dynamic pool into ~1.88 MiB + ~4.61 MiB segments. A single contiguous DATA carve is capped at ~4.6 MiB. All 31 marketplace plays with DATA manifests require ≥5.7 MiB total and **cannot install at declared size**; full A/B coexistence of two market DATA plays is arithmetically impossible. `play28-recordings-4m` (≈5.5 MiB total) also exceeds one segment; only the new 3 MiB variant installed successfully, verified on hardware.
2. **Legacy uninstall deadlock.** A legacy `play_id=0` APP can never be removed while any DATA entry exists (fail-closed by design), and orphaned/archived DATA from earlier failed runs is never reclaimed. Recovery required erasing only the carve records (`esptool erase_region 0x35A000 0x2000`); NVS identity and Wi-Fi credentials were untouched.
3. **First phone-page-driven market DATA install.** Play 28 + `play28-recordings-3m` installed end-to-end through the real device page: analyze → capacity picker → prepare → session → chunks → finalize → carve commit, with the device independently reporting the 3 MiB `recordings` reservation.

## 6. Remaining gap → full E2E

Publishing two dedicated `data_child` plays (app ≈0.17 MiB, well inside the segment cap, both coexist, full serial protocol) is the only remaining path to the complete M01–M08 E2E with runtime DATA evidence. That requires a creator-center one-time authorization code.
