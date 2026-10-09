<p align="right">
  <strong>English</strong> · <a href="mobile-page-storage-e2e-design.zh_CN.md">简体中文</a>
</p>

# Mobile Embedded Installer: Storage Management and Child-Firmware E2E

Date: 2026-10-09  
Branch: `feat/storage`

## Goal

Use a browser at a phone-sized viewport to operate the device-hosted installer page. Independently verify outcomes through read-only device APIs and device-side serial evidence. A viewport browser test is not interchangeable with an emulator, and a UI success toast alone is not proof of a committed device install.

Current runner: `tools/realdevice/mobile_page_e2e.mjs`  
Runtime driver: `tools/realdevice/mobile_page_runtime_driver.py`  
Child-firmware serial client: `tools/realdevice/data_child_serial.py`

## Implemented real-device path

| Phase | Automated action | Required postcondition |
|---|---|---|
| M01 page and viewport | Open the real device page; check content, controls and touch; test 320×720, 360×800, 390×844 and 430×932 | Usable page, visible controls, no horizontal overflow, no page/console/request errors |
| M02 baseline safety gate | Read slots, DATA reservations and installer status | Installer idle; pool bounds, alignment, granularity, overlap and free-byte accounting agree; otherwise refuse mutation |
| M04 install A | Search the marketplace by play ID, select a slot and install; independently read device state | UI completion plus VALID slot in device API; valid geometry; existing DATA reservations preserved |
| M04A cancel-uninstall guard | After installing A, enter management and cancel the delete confirmation | Slot table, DATA reservations and installer state remain unchanged; otherwise fail before further destructive operations |
| M04B child A | Run the device-side runtime driver | Child actually boots; erase, write, readback, checksum and reboot persistence have serial evidence |
| M05 install B | Install a second play ID | A and B coexist; no overlap; baseline DATA reservations preserved |
| M05C isolation | Compare DATA physical addresses reported by the device driver | A/B DATA extents differ; different play IDs alone do not prove isolation |
| M06 remove A | Delete through management UI with confirmation; run B after deletion | A absent; B remains VALID and boots; A DATA reservation released; geometry remains valid |
| M07 remove B | Delete through UI and verify | B absent; device evidence proves deleted image is not bootable and DATA is released |
| M08 restore baseline | Re-read slots, reservations and status | Installer idle; slots, reservations and free bytes exactly match baseline |

## Distinct assertion layers

1. **UI:** management list, slot state, delete confirmation, install progress and completion/failure.
2. **Device API:** independent reads of `/api/install/slots` and `/api/install/status`.
3. **Allocator:** every app slot and DATA reservation is inside the dynamic pool, correctly aligned, non-overlapping, and `free = pool bytes - occupied bytes`.
4. **Runtime:** the child actually boots and provides USB Serial/JTAG evidence for DATA erase, write, readback, checksum, reboot persistence and return to launcher.

A vanished UI row does not prove a flash image cannot boot. A missing DATA reservation does not prove the child's DATA contents were validated.

## Safety and reproducibility

- Hardware mutation requires explicit `--real-device`; destructive tests must never run by default.
- Only create uniquely named test slots and never remove baseline user slots.
- If the installer is busy, slot geometry is invalid, or a test name collides, stop instead of attempting automatic repair.
- On failure, clean up only slots created by this run and record cleanup failures.
- Logs must not contain session tokens, full device URLs, LAN IPs or sensitive host paths.
- `--manual-assist` may only report `PASS_WITH_MANUAL_STEPS`; DATA persistence and deleted-image non-bootability without device evidence must remain uncovered.
- This is an explicitly dispatched hardware test, not an automatic flash operation in ordinary CI.

## Evidence boundary

The runner already contains M01–M08 UI, device API and runtime-driver steps, including optional reservation checks, A/B physical isolation, reservation release and baseline restoration. A static contract test is added to prevent regressions in those gates.

This does not mean a real-device E2E ran in this turn. Real results must come from the redacted evidence in `tools/realdevice/logs/<run>/report.json`. Passing the synthetic test firmware must not substitute for market-firmware DATA analysis.
