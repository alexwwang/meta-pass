<p align="right">
  <a href="mobile-page-data-e2e-test-firmware.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Dedicated child firmware for mobile-page DATA E2E

Status: implementation baseline on `feat/storage`; hardware integration still pending.

## Goal

Prove the real child-firmware DATA path rather than infer it from installer HTTP responses. The test image is a small ESP-IDF 5.5.3 application that accesses its declared DATA partition through `esp_partition_find_first()` and `esp_partition_*` APIs. It does not write arbitrary flash offsets.

The same binary may be installed as test play A and test play B under different play IDs. The launcher materializes DATA partitions for the active play ID, so identical labels can resolve to different physical extents. Tests must use distinct nonces and record each play's reported DATA offset/size to prove isolation.

## Repository layout

- `tests/realdevice/data-child/CMakeLists.txt`: standalone ESP-IDF project.
- `tests/realdevice/data-child/partitions.csv`: template image declares a 64 KiB `e2edata` DATA partition; installation reserves it in the dynamic pool.
- `tests/realdevice/data-child/main/test_main.c`: line-oriented USB Serial/JTAG protocol and DATA operations.
- `tools/realdevice/data_child_serial.py`: host-side serial protocol client / evidence collector.
- `docs/assets/mobile-page-data-e2e-test-firmware.zh_CN.md`: Chinese runbook.

## Device protocol

USB Serial/JTAG uses newline-delimited ASCII commands; each response is a single JSON object. Commands are intentionally narrow and do not accept flash offsets.

| Command | Device-side behavior |
|---|---|
| `HELLO` | Report protocol/build and running app identity |
| `INFO` | Find `e2edata` by label and report partition address/size |
| `WRITE <32-hex-nonce>` | Erase the first DATA sector and write a versioned deterministic record using the resolved partition handle |
| `READ` | Read the record from DATA and return nonce, sequence and SHA-256 |
| `ERASE` | Erase only the test record's first sector |
| `REBOOT` | Restart the ESP; launcher policy is expected to return to factory on next boot |

The protocol is test-only and must never be enabled in production firmware. USB serial output is evidence only when the record is read by the child after the real launcher/bootloader lifecycle.

## Required end-to-end sequence

1. Capture the baseline launcher slot/data listing.
2. Install the test image as A and B through the actual mobile page. The install manifest must declare `e2edata` and its size; the device must allocate one DATA record per play ID.
3. Launch A through the real launcher/bootloader path. Record `INFO`, issue `WRITE <nonce-A>`, then `READ`; require exact nonce and SHA-256 match.
4. Return to launcher, launch B, repeat with a different nonce. Confirm B's DATA offset differs from A's and the record is independent.
5. Return to launcher, reboot/relaunch A and require nonce-A to persist; repeat for B. A host acknowledgement or launcher listing is not persistence evidence.
6. Remove A through the page. Verify A's slot is absent/non-bootable and its DATA record is released/archived according to the current removal policy; launch B and prove nonce-B still reads correctly.
7. Remove B and verify the final slot/data geometry returns to baseline.

## Current integration boundary

The test firmware exercises DATA access once it is running. It does not itself make the launcher select a slot. The existing `mobile_page_e2e.mjs` runtime-driver contract requires `boot-test-and-return` and `verify-deleted` actions; a serial client alone cannot safely perform these actions because this branch now includes a `CONFIG_META_E2E_TEST_CONTROL`-gated launcher USB control channel and `mobile_page_runtime_driver.py`. The channel accepts only an existing, scanned bootable slot index, reuses `meta_store_boot_slot()`, and exposes read-only slot/DATA listings. After removal, the driver also probes the old play ID and requires the launcher to reject it as non-bootable. It defaults to `n` and must remain disabled in production builds. Neither the firmware nor the control channel has yet been built with ESP-IDF or validated on hardware, so full hardware E2E is not accepted yet.

Build the test launcher with the overlay below; never use this overlay for release images. Real-device acceptance must still confirm that child reboot returns to launcher and that USB Serial/JTAG console input works with the target IDF configuration.

## Build

Launcher test-control overlay (only for a dedicated test build):

```bash
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.e2e-test.defaults' reconfigure
idf.py build
```


Use an ESP-IDF 5.5.3 environment and build from this directory:

```bash
idf.py set-target esp32c3
idf.py build
```

The generated `build/data_child.bin` is an app image for the normal meta-pass install/extract path, not a full-flash image. Confirm that the marketplace/analyze manifest declares `e2edata`; the app binary alone does not carry DATA contents.

## Use with the mobile-page E2E runner

Enable the test overlay on a dedicated launcher build and deploy it using the repository's protected-layout workflow. Never use the test configuration for releases or overwrite the protected device-identity partition with a full-flash image. Publish/provide the same test child binary under two distinct test play IDs; both analyze manifests must declare `e2edata`.

```bash
python3 -m pip install pyserial
export META_PASS_E2E_SERIAL_PORT=/dev/cu.usbmodemXXXX  # replace with the actual local port
node tools/realdevice/mobile_page_e2e.mjs \
  --real-device \
  --cdp-url http://127.0.0.1:9222 \
  --url 'http://<device-ip>/' \
  --token '<32-hex-session>' \
  --runtime-driver tools/realdevice/mobile_page_runtime_driver.py \
  --require-data-reservation \
  --play-a <test-play-id-a> --play-b <test-play-id-b>
```

This run installs/removes the plays through the real mobile page and uses the USB serial driver to launch the child, write/read DATA, reboot, relaunch, and verify persistence. A full E2E PASS is possible only if the device control channel, child protocol, page operations, and final baseline restoration all pass. The chain still needs an ESP-IDF 5.5.3 build and physical-board validation.

## Evidence / limitations

- Firmware responses report actual partition address/size and bytes read back by the child.
- Host SHA-256 is independently recomputed from returned record bytes where available.
- A `REBOOT` command by itself is not proof that data survived a reboot. The child must be launched again and `READ` must match.
- Do not use `esptool write-flash` to simulate child DATA access; that bypasses the production data-partition path.
- Do not use manual checkpoints as a substitute for the automated verdict. Manual-assisted runs must remain `PASS_WITH_MANUAL_STEPS`.
