[简体中文](HANDOFF-usb-v1-archive-compatibility.zh_CN.md)

# Handoff: USB historical v1.1 archive compatibility regression

Branch: `feat/storage`. Never develop on `main`.

## Purpose and scope

Validate the USB service page's retained historical v1.1 fixed-slot APP/NVS archive path on the current checkout. This handoff does **not** ask for v1 support in the phone embedded page. USB/phone feature alignment does not imply identical archive-version support.

Current verified baseline: `95eea1911646eefcc5ab679317f7929195ca4601`; Static checks and Firmware checks passed for this commit. Always record the actual HEAD and rerun `./tools/validate.sh` before hardware testing if HEAD has advanced.

## Safety rules

- Do not move `cardid`, change the partition layout, use guessed Flash addresses, or bypass the allocator.
- Do not run the v1 restore path on a dynamic-v2 device. The page must reject the archive before any write.
- A v1.1 archive may contain a raw NVS image, including Wi-Fi credentials and device state. Keep it local, restrict access, and never commit/upload it.
- Do not use a production device for destructive restore unless the owner has explicitly approved overwriting its APP slot and NVS and has a verified recovery route.
- Do not interpret a successful ZIP download as proof of a complete archive; inspect the manifest and payload hashes.

## Stage A — host checks and source contract

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
git rev-parse HEAD
./tools/validate.sh
```

Confirm the current USB page and tests enforce all of the following:
1. The USB restore input accepts a validated `manifest_version: 1` archive with kind `meta-pass-slot-backup`.
2. The v1 restore handler is USB-only and fixed-slot-only.
3. Dynamic-v2 devices reject v1 restore before Flash writes.
4. The legacy NVS payload length and SHA-256 are checked against the manifest before writing.
5. If the archive contains NVS, the legacy handler locates the device NVS partition from the live partition table and writes the historical raw NVS payload to that partition.
6. `manifest_version: 2` APP+DATA logical archives are rejected; `meta-pass-storage-snapshot` archives remain gated because safe USB snapshot restore is not implemented.
7. The phone embedded page remains v2 whole-device-snapshot-only. Do not change it to accept v1.

## Stage B — classify available hardware

Connect the device and record firmware SHA-256, partition table, detected storage mode, APP/slot listing, and whether it is fixed-slot or dynamic-v2.

- On a dynamic-v2 device, run the negative test only: load a known-good historical v1.1 archive and confirm a clear fixed-slot-only error. Capture page logs and verify the APP/DATA listing and device state are unchanged. Do not click through any alternative path or attempt a write.
- Only if a known fixed-slot device and an approved recovery plan are available, proceed to Stage C.

If no fixed-slot device is available, report the positive legacy restore test as `BLOCKED_HARDWARE_UNAVAILABLE`; do not mark it PASS based on source inspection or a dynamic-device rejection.

## Stage C — fixed-slot v1.1 round trip (only on approved legacy hardware)

1. Create a fresh USB-page v1.1 backup. Keep the ZIP local and record its SHA-256.
2. Validate `manifest.json`: `manifest_version=1`, `kind=meta-pass-slot-backup`; all referenced APP/NVS payloads exist, lengths match, and SHA-256 values match.
3. Record the source slot layout and NVS partition geometry from the live device.
4. Restore the archive to the same fixed-slot device through the USB UI. Do not call internal APIs directly.
5. Capture the UI result and serial/loader logs. Verify restored slot contents with a read-only dump and byte comparison against archive payloads where the archive semantics specify raw bytes. Verify NVS restoration only with an explicitly approved procedure; do not expose its contents in the report.
6. Reboot normally. Verify the device boots, its expected fixed-slot inventory is present, and required connectivity works. If NVS restoration changes Wi-Fi configuration, follow the approved recovery plan.
7. Verify the device remains in fixed-slot mode and no partition-table or `cardid` location changed.

Any failure must stop further destructive actions. Preserve sanitized logs and report the exact phase, first error, and whether Flash writes had begun.

## Evidence and report

Create `tools/realdevice/logs/<timestamp>-usb-v1-archive-compat/` with:
- `report.md`, `report.json`, and `environment.txt`;
- actual branch/commit, firmware SHA-256, tool/browser versions, and detected storage mode;
- manifest summary and archive/payload hashes (not raw secrets);
- before/after partition and slot listings;
- page console/logs, loader output, reboot/boot evidence, and read-only byte-compare results;
- overall status and one of: `PASS`, `FAIL`, `BLOCKED_HARDWARE_UNAVAILABLE`, `BLOCKED_RECOVERY_PLAN`.

Keep ZIPs, NVS images, Wi-Fi credentials, pairing tokens, and device secrets out of Git and the final report. Do not claim positive v1 restore passed unless Stage C was executed on fixed-slot hardware.
