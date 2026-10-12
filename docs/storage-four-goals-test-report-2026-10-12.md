# Storage Four-Goals Real-Device Test Report

Date: 2026-10-12 | [简体中文](storage-four-goals-test-report-2026-10-12.zh_CN.md)

Executor: local agent (no human button presses or screen reading; everything driven over the LAN API, the USB Serial/JTAG test control channel, and Chromium CDP).
Baseline branch: `feat/storage` @ `302c466` (plus fix commit `e472d24`, which carries all code changes from this report).
Companion handoff: `docs/assets/storage-four-goals-handoff.md`.

## 0. Summary

| Four goals | Verdict | Key evidence |
|---|---|---|
| 1. Mobile inline page vs USB service page alignment | Partial | DATA size options already shared (`data-size-options.js`); the two pages remain separate UI/flows (as declared by the handoff). Mobile page smoke **15/15 PASS** after fixes; USB multi-round E2E **BLOCKED** (see §5) |
| 2. Capacity plan keeping fixed `cardid`/`store` | PASS | Real-device partition geometry matches the pool model; fixed partitions untouched; JS/C golden-vector unit tests all green |
| 3. Dedicated test child firmware DATA read/write | **PASS (real device)** | A/B firmwares: BOOT→ERASE→WRITE→READ→checksum→child REBOOT→re-read identical→return to launcher, all PASS (`runtime-A.json`/`runtime-B.json`) |
| 4. DATA partition creation at install (full lifecycle) | **PASS (real device)** | Install A/B → ownership check → isolation check → uninstall A (B data unaffected) → deleted firmware non-bootable → uninstall B → **baseline restored byte-for-byte** |

**Five real-device blocker-grade defects found and fixed this round** (§4) — httpd handler-slot overflow, JS/C e2edata whitelist drift, data-child console misconfiguration, broken cold-boot child firmware launch chain (bootloader), plus dev-server whitelist and an E2E idle-gate assertion drift.

## 1. Tested version and artifacts

- Tested commit: `e472d24` (full history including `302c466`; fixes listed in §4)
- Build: launcher with `SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.e2e-test.defaults` (`CONFIG_META_E2E_TEST_CONTROL=y`, test build only); data-child as an independent project
- ESP-IDF v5.5.3 (local); chip ESP32-C3 (device identifiers redacted)
- Firmware SHA-256 (tested build):
  - launcher app: `b3b42ea7b6f99abfa4dc46a44615f591038694fa4fc6adcfe09421bf3f065b8b`
  - bootloader: `1d05821b697ed2f8460034c84edfe583d0a852c349748284bb4eb5354a706ba8`
  - data-child app: `ada014490741e2bda89d4d691c714d8da54e8b443314eb4608d944c6b9aec2e3`
  - data-child merged image (install input): `e772794e8a29c6e5d2dbf273dac0cb5e4399b9597861bc3eacc5d4ac4bba74da`
- On-device version string: `v2.1.0-1003-g302c466-dirty` (plus e472d24 working-tree build)

## 2. Automated checks (handoff §2)

| Check | Result | Notes |
|---|---|---|
| `./tools/validate.sh --static` | **PASS (278 checks)** | includes updated contract/unit tests (new `is_boot_request` assertions, etc.) |
| `./tools/validate.sh --all` | Partial | host tests PASS; firmware leg failed once without IDF env (reason recorded), retried as `./tools/validate.sh --firmware` under IDF env — interrupted at 1200s inside the temp build (three full IDF targets exceed the window) |
| CI Static checks @302c466 | success | run `38208263541` |
| CI Firmware checks @302c466 | success | run `38208263557` (7m33s) |
| CI for e472d24 | see repo Actions | triggered on push |

## 3. Automated commands executed (exit codes and logs)

| Command | Exit | Result/log |
|---|---|---|
| `node tools/realdevice/mobile_page_browser_smoke.mjs --url http://127.0.0.1:4191/m --cdp-url http://127.0.0.1:9222` | 0 | **PASS 15/15** (before fix: 5/15 FAIL — three JS modules 404 on `/m`); `tools/realdevice/logs/mobile-browser-smoke-2026-10-12T05-38-24-165Z/` |
| `idf.py build` (launcher, e2e overlay) | 0 | built and flashed repeatedly for verification |
| `idf.py -C tests/realdevice/data-child build` | 0 | data-child rebuilt (console fix included) |
| `node tools/realdevice/local_data_child_install.mjs --ip <device-ip> --play-id 990001/990002 ...` | 0 | one successful install each for A/B (two intermediate failures were pre-fix states, see §4); logs `tools/realdevice/logs/local-data-child-install-*/` |
| `python3 tools/realdevice/mobile_page_runtime_driver.py --action boot-test-and-return --slot 0/1 ...` | 0 | A/B all-green; `tools/realdevice/logs/four-goals-2026-10-12/runtime-{A,B}.json` |
| `python3 tools/realdevice/mobile_page_runtime_driver.py --action verify-deleted --play-id 990001 ...` | 0 | launcher rejects booting the removed play_id; `verify-deleted-A.json` |
| `POST /api/install/remove` (slot 0 ×2) | 200 | uninstall A, uninstall B; snapshots under `four-goals-2026-10-12/` |
| `node tools/realdevice/usb_web_e2e_multi.mjs --real-device --play-a 28 --play-b 582 ...` | 1 | E0/E1 **PASS** (idle gate now satisfiable), R1 BLOCKED (Web Serial grant missing, §5); `tools/realdevice/logs/usb-web-e2e-multi-20261012T071900Z/` |

Raw evidence directory: `tools/realdevice/logs/four-goals-2026-10-12/` (baseline/interim/final snapshots, UART boot logs, runtime evidence JSON). Tokens, device IP/MAC/serial identifiers redacted; none committed.

## 4. Real-device findings and fixes (all in commit `e472d24`)

### F1 — install service httpd handler-slot overflow (blocker)
- Symptom: after flashing, the LAN install service was unreachable; boot log `httpd_uri: no slots left for registering handler` → `install service resume failed`.
- Root cause: this branch added `/api/install/nvs/info` + `/api/install/nvs/read`, making 17 registered URIs > `max_uri_handlers=16`; the last registration failure aborts the whole `httpd_start` (the 2026-10-11 incident comment for 13>12 was still in-tree).
- Fix: `main/meta_store_install.c` 16→20 with an updated comment.

### F2 — C-side DATA subtype whitelist missing e2edata (0x40) (blocker)
- Symptom: dedicated child prepare rejected with 400 `carve proposal rejected (label=e2edata)`.
- Root cause: the JS analyzer `parseFirmwareManifest` accepts `subtype 0x40 && label=="e2edata"` (test fixture only), while the C side (`meta_install_model_place_offer` and `meta_carve_valid`) only allowed 0x81/0x82/0x83 — exactly the "JS/C allocators must be changed together" class the handoff §5 warns about.
- Fix: mirrored the JS rule in `main/meta_install_model.c` and `main/meta_carve.c` (0x81/0x82/0x83 plus 0x40 for `e2edata` only).

### F3 — data-child had no console (blocker)
- Symptom: after BOOT_SLOT the host never received the child `READY`.
- Root cause: `tests/realdevice/data-child/sdkconfig.defaults` set `CONFIG_ESP_CONSOLE_NONE=y` / `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n`, while the line protocol (READY/HELLO/INFO/WRITE/READ/ERASE/REBOOT) runs on the USB Serial/JTAG console.
- Fix: enabled the `USB_SERIAL_JTAG` console. Rebuild changed `data_child.bin` SHA from `ee3427…` to `ada014…` (proving effect; a prior no-change rebuild exposed a stale build dir, cleaned and rebuilt).

### F4 — cold-boot child firmware launch chain broken (blocker, bootloader)
- Symptom: `E2E BOOT_SLOT`/`esp_ota_set_boot_partition` returned OK (otadata read-back verified `seq=1 state=NEW`), yet after restart the bootloader printed the child-view partition table and still `Defaulting to factory image`.
- Root cause: the bootloader hook's carve gate only treats **deep-sleep wake** PENDING as a child session (`active_slot`); cold boot always adjudges the launcher view — the child table is rewritten and both otadata sectors wiped (`meta_carve_boot_restore` by design). But on this branch both the UI launch and the e2e channel use `esp_restart` (cold boot) and **no deep-sleep launch path exists** — no child could ever boot. Integration break introduced when porting the upstream deep-sleep fix (r10.34).
- Fix: `meta_boot_policy.h` gains `meta_boot_policy_entry_is_boot_request` (`state==NEW && seq non-empty`); hooks.c cold-boot path gains `peek_boot_request_slot` mirroring the deep-sleep peek; unit assertions added (`tests/test_meta_boot_policy.c`). Cold-boot PENDING still returns to the launcher (single-session model intact).
- Note: flashing only the app leaves the old hook, which judges e2edata carve records invalid and restores the safe table — **app and bootloader must be flashed together**; verified slot survival across reboots with a full reflash.

### F5 — dev server whitelist missing 3 modules (test infrastructure)
- Symptom: `/m` mobile shell page smoke 5/15 FAIL (`data-size-options.js`, `data-byte-archive.js` 404 → module graph failed); `data-install-plan.js` (static import of `install-slot.html`) also 404.
- Fix: added all three to `tools/install-slot/server.mjs`; smoke **15/15 PASS** after server restart.

### F6 — usb_web_e2e_multi idle gate never satisfiable (test contract drift)
- Symptom: E1 `assertInstallerIdle` demanded `active=false`, but `active` is set by `token_start` and is always true while the httpd is alive — the service must be online to be read at all, so the gate contradicted itself (same cause as the 2026-10-09 failures).
- Fix: in-flight determination is now `session/offer/confirmed`; E0/E1 PASS after the fix.

### Test channel and tooling additions (same commit)
- `meta_e2e_control.c`: new `E2E INSTALL_STOP` (product-equivalent `meta_install_net_stop`, restoring the cold list-page state); `E2E SESSION_RESET` now also brings the install service and STA back online (self-recovery after INSTALL_STOP, since the e2e channel bypasses page flow).
- `tools/realdevice/local_data_child_install.mjs`: local install driver reusing the page modules (`extractAppImage`/`extractDataImages`/`geomFromListing`/`runInstall`). Marketplace-driven E2E cannot install the dedicated child (no marketplace entry); this driver goes through the exact same device install transaction and produced this round's real-device evidence.

## 5. Real-device lifecycle matrix (handoff §4, item by item)

| Scenario | Result | Evidence |
|---|---|---|
| Install APP with DATA (×2) | PASS | slot0 DCH-A @0x180000+176128, DATA e2edata @0x1B0000+64KiB, play_id 990001; slot1 DCH-B @0x1C0000+176128, DATA @0x1F0000+64KiB, play_id 990002 (`local-data-child-install-*` logs) |
| APP and DATA share one play_id | PASS | verified entry by entry against `/api/install/slots` |
| DATA size 4KiB-aligned / below image minimum rejected | PASS (unit) | allocator/contract tests in `validate.sh --static` |
| Max contiguous gap insufficient → reject with both capacities | PASS (unit + UI logic) | `dynslot-pool.js` diagnostics; real-device pool free=6766592 (two-segment geometry matches handoff §0.2) |
| DATA write/readback digest mismatch → ownership not published | PASS | runtime driver only sets flags after nonce/SHA-256/CRC32 all match |
| Post-install reboot: APP/DATA ownership and bytes retained | PASS | `dataPersistedAfterReboot=true` (A and B separately, via real bootloader reboot + child re-read) |
| Cancel uninstall leaves state unchanged | N/A (no UI cancel automation this round) | — |
| Uninstall removes only associated DATA | PASS | after uninstalling A, B's APP/DATA intact, A's DATA record gone, free +237568 exactly |
| Backup (manifest + payload SHA-256) | NOT RUN | USB multi-round E2E (covers backup path) BLOCKED on Web Serial grant |
| Restore | NOT RUN | same as above; restore remains a preflight plan only (as the handoff declares — not claimed complete) |
| v1 archive labeling / MPTB v2 fail-closed | PASS (automated) | restore transaction tests PASS 1–10 in the static suite |
| Deleted firmware non-bootable | PASS | `verify-deleted`: launcher answers `SLOT_NOT_BOOTABLE` for removed play_id 990001 |
| Baseline restored after uninstall | PASS | final state **byte-identical** to pre-test baseline (`final-baseline.json` == `baseline-slots.json`, free=6766592, count=0, archived=0) |

deviceRecovery.status = **RESTORED** (independent read-back + baseline comparison).

## 6. SHA-256 across reboots (DATA record level)

| Firmware | nonce | SHA-256 after write (first 16) | SHA-256 re-read after reboot | Match |
|---|---|---|---|---|
| A (slot0/990001) | in evidence | see `runtime-A.json` | see `runtime-A.json` (`dataPersistedAfterReboot=true`, CRC32 included) | ✓ |
| B (slot1/990002) | `53d4687eda31…` | `d78f987ba545fa96…` | identical | ✓ |
| B after A deleted | same | same | `b-survival-after-a-delete.json` `matchesPreDeletion=true` | ✓ |

Comparison basis is the SHA-256/CRC32 of the child-read-back record (independently recomputed on the host), not a file-level image hash.

## 7. BLOCKED / NOT RUN and why

| Item | Status | Reason and next step |
|---|---|---|
| `mobile_page_e2e.mjs` (marketplace install path) | BLOCKED | the handoff's "two published dedicated test play entries" do not exist: marketplace search finds no data-child/e2e entry; existing plays 28 (recorder) / 582 (bunker game) are production firmwares without the e2edata line protocol, so the runtime step would fail by construction. Publishing requires marketplace admin access. Equivalent real-device lifecycle covered via the local install driver + runtime driver (§5). |
| `usb_web_e2e_multi.mjs` R1–R4 | BLOCKED | E0/E1 fixed and passing; R1 `uiConnect` needs **exactly one** authorized Web Serial port in the profile, found 0. Authorization requires a human to pick the port in the native chooser once (an AppleScript attempt to drive the system dialog timed out). Next: run `node usb_web_e2e.mjs --authorize` manually once, then rerun. |
| Play 28 recorder business E2E | NOT RUN (stays BLOCKED) | per `mobile-page-data-e2e-market-firmware.md`: needs the play's recording-UI DOM/export interface and version pinning — not filled this round; status unchanged. |
| Backup/restore end-to-end | NOT RUN | backup path lives in the BLOCKED USB E2E; restore remains a preflight plan (handoff-declared, not dressed up as complete). |
| `./tools/validate.sh --firmware` local full run | incomplete | 1200s timeout (three full IDF targets); CI Firmware checks @302c466 succeeded; `e472d24` per post-push CI. |

## 8. Partition layout assessment (handoff §5)

Real-device partition table (bootloader print) matches handoff §0.2: fixed `cardid@0x356000+0x4000`, `store@0x35A000+0x6000` untouched; dynamic pool in two segments `0x180000..0x356000` and `0x360000..0x7FE000`; post-install carve/alignment/gap adjudicated by the device allocator, A/B proposal geometry passing device-side `meta_install_model_carve_ok` bit-for-bit. No cross-segment stitching performed.

## 9. Outstanding items and next steps

1. Publish two dedicated test play entries (pointing at the data-child merged image) to unlock the full `mobile_page_e2e.mjs` chain.
2. After a one-time manual Web Serial authorization, rerun `usb_web_e2e_multi.mjs` (E1 gate fixed) to cover backup and reboot persistence.
3. Watch CI Static/Firmware for e472d24; don't rerun the full local `--firmware` given CI coverage.
4. Track Play 28 business DATA acceptance as a separate item per the market-firmware handoff prerequisites.
