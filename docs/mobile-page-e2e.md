<p align="right">
  <a href="mobile-page-e2e.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Device Phone-Install Page and Child-Firmware Lifecycle E2E

Target branch: `feat/storage`.

## 1. Goal

This is not merely a page-rendering or read-only API test. It must install and delete firmware through the real device's web UI, boot the installed child firmware on hardware, access its DATA partition, verify write/read/checksum behavior and persistence across reboot.

The current UI runner uses Playwright mobile viewport emulation. It does not require an Android emulator, but it is not a native WebView-container test. Device GET APIs are independent verification only; install/delete write APIs must never bypass the UI.

## 2. Required components

1. **Real ESP32 device** running the launcher firmware under test and serving the phone page.
2. **Browser UI automation** in one persistent mobile viewport session. Create/install/delete actions must be triggered through page controls.
3. **Dedicated test child firmware A/B**: both images must boot on the target hardware and implement deterministic DATA-partition read/write tests. Arbitrary marketplace firmware cannot be assumed to expose test markers or DATA behavior.
4. **Hardware runtime driver**: use actual device control and device-side observations to boot a slot, wait for test firmware execution, return to the launcher, and collect evidence. Mock API responses are not acceptable. Supply it with `--runtime-driver`; contract below.
5. **DATA support**: both A and B must declare and actually access supported DATA partitions. The full lifecycle test must fail rather than silently skip read/write assertions if they do not.

## 3. Stages and acceptance criteria

| Stage | Real action | Pass condition |
|---|---|---|
| M01 | Open the device page at mobile viewport sizes | Page module mounted, key controls present, no horizontal overflow or unhandled JS errors |
| M02 | Capture slot/installer baseline | State readable and installer idle; record slots, DATA reservations and free space |
| M03 | Search A, choose slot/name and confirm installation | Entire install triggered through UI; device confirmation flow completes |
| M04 | Boot A and run runtime checks | Driver proves A booted; A writes DATA, reads it back and verifies content; data persists after reboot; returns to launcher |
| M05 | Install B through UI, then boot B | B passes the same runtime checks; A/B slots and DATA extents do not overlap |
| M06 | Delete A through Space Management UI | A removed; B remains valid and is booted again to prove its runtime and DATA still work |
| M07 | Verify post-delete hardware state | Device-side evidence confirms A is no longer bootable and its DATA extent/reservation is released; verification must not perform deletion |
| M08 | Delete B through UI and inspect final state | B is no longer bootable, its DATA extent is released, and slots/DATA/free space return to baseline |
| M09 | Failure evidence | Report, screenshot, page source, raw runtime-driver evidence and failure reason are saved and redacted |

Slot listings and DATA reservation APIs are supporting evidence, not proof that a child firmware actually ran. Runtime evidence must originate from the device (for example, serial test markers, hardware reset/button action logs, and the child's DATA self-test results).

## 4. Hardware runtime driver contract

The runner invokes an external executable; it does not assume an undocumented remote button API exists:

```bash
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device \
  --url 'http://<device-ip>/' --token '<32-hex-session>' \
  --play-a <test-play-id-a> --play-b <test-play-id-b> \
  --runtime-driver /absolute/path/to/device-runtime-driver
```

Each runtime run is invoked with:

```text
--action boot-test-and-return
--slot <slot-index> --play-id <play-id> --slot-name <unique-name>
--phase <phase> --timeout-ms <milliseconds> --evidence-file <path>
```

The driver must use real hardware control to: boot the selected slot → observe the child boot marker → instruct the test firmware to write a unique nonce/payload to its DATA partition → read it back and verify bytes/checksum → reset the device and verify persistence → return to the launcher using a real device operation → confirm the launcher is running again. Ordinary page JavaScript must not simulate these steps.

The driver writes JSON to `--evidence-file`. Each `boot-test-and-return` result must include these boolean fields set to `true`:

- `childBooted`
- `dataWriteOk`
- `dataReadOk`
- `dataChecksumOk`
- `dataPersistedAfterReboot`
- `returnedToLauncher`

It should also include `dataLabel`, `serialEvidence` (a path or digest for device-side raw evidence), and traceable phase information. After deleting A, the runner invokes a read-only verification action:

```text
--action verify-deleted --slot-name <unique-name> --play-id <play-id>
--timeout-ms <milliseconds> --evidence-file <path>
```

Its evidence must include `deletedSlotNotBootable: true` and `dataPartitionReleased: true`. The driver's `verify-deleted` action may observe and attempt non-destructive verification only; it must not delete a slot or modify allocator records on behalf of the runner.

**Important: this repository currently defines and invokes the driver contract but does not include a generic implementation for any specific UART/relay/button-control hardware.** Until an actual hardware driver and dedicated test firmware A/B are provided, this test cannot be reported as a complete E2E PASS. If the setup lacks automated button/reset hardware, the real control mechanism must be identified explicitly rather than guessed.

## 5. Safety constraints

- Explicitly pass `--real-device` and `--runtime-driver`.
- Use dedicated test play IDs A/B, distinct from each other; slot names are unique per run.
- Install/delete only through page UI. The runner must not call install prepare/session/chunk/finalize/remove write APIs.
- Read-only APIs are limited to cross-checking slots, DATA allocation and installer status.
- Failure cleanup may remove only exact unique names created by this run; stop if target identity is ambiguous.
- Do not leak session tokens, device IPs or credentials into reports; preserve redacted screenshots, page source and device-side evidence.

## 6. Verification boundary

Static contract tests and JavaScript syntax checks validate the test harness, not the device behavior. Without a connected device, real hardware driver, dedicated test firmware and collected device-side evidence, device E2E must remain NOT RUN/FAIL; passing read-only API checks is not a substitute.
