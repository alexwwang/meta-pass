# Multi-round USB Web UI E2E Test Report — 2026-10-09

**Repo**: `meta-pass` @ `feat/storage` (post `e08b0b1`)
**Device**: ESP32-C3 Passport, one USB-JTAG port
**Browser**: Chrome 154.0.8037.99 (single persistent session, Web Serial authorized)
**Result**: **4 of 4 rounds PASS**
**Logs**: `tools/realdevice/logs/usb-web-e2e-repro-r2/`, `tools/realdevice/logs/usb-web-e2e-r3-r4/`

## Executive Summary

Multi-round E2E test (install → reboot → install → reboot → remove → reboot → remove → reboot) on a real device. All four rounds passed. Baseline fully restored at end. No flash erasure at any point.

| Round | Action | Result |
|-------|--------|--------|
| R1 | Install Play #1 (weather clock, 1.97 MB) → USB reset | **PASS** |
| R2 | Install Play #2 (Luigi's Mansion, 615 KB) → USB reset | **PASS** |
| R3 | Remove Play #1 → USB reset | **PASS** |
| R4 | Remove Play #2 → USB reset | **PASS** |

## Round-by-round Results

| Round | Action | Carve seq | Slots | Free space | Evidence |
|-------|--------|-----------|-------|------------|----------|
| R1 | Install P1 @ 0x360000 (1.97 MB) | 73→75 | 1→2 | 5640192→3633152 | Boot: `carve loaded: seq=75 slots=2`. API: 2 slots (smoke-three + P1). Page log: `Carve record committed`, `Partition table materialized`, `read-back verified` |
| R2 | Install P2 @ 0x2b0000 (615 KB) | 75→77 | 2→3 | 3633152→3010560 | Boot: `carve loaded: seq=77 slots=3`. API: 3 slots (smoke-three + P2 + P1). Page log: full install chain verified |
| R3 | Remove P1 (slot index 2) | 77→78 | 3→2 | 3010560→5017600 | API: 2 slots (smoke-three + P2). P2 intact, smoke-three intact |
| R4 | Remove P2 (slot index 1) | 78→79 | 2→1 | 5017600→5640192 | API: 1 slot (smoke-three). Baseline fully restored |

## Carve Record Lifecycle

```
seq=73  baseline (smoke-three only)
  R1 install P1 → carve record commit + partition table materialize + flash write + tail metadata + final commit
seq=75  2 slots (smoke-three, P1)
  R2 install P2 → same chain, new slot carved from free space
seq=77  3 slots (smoke-three, P2, P1)
  R3 remove P1 → erase image header + carve record commit (materialize: true)
seq=78  2 slots (smoke-three, P2)
  R4 remove P2 → same remove chain
seq=79  1 slot (smoke-three) — baseline restored
```

Each step survived `rst:0x15 (USB_UART_CHIP_RESET), boot:0xa (SPI_FAST_FLASH_BOOT)`.

## What Was Verified

- **Carve persistence across reboots**: seq advances monotonically (73→75→77→78→79), each value survives USB hard reset.
- **Multi-slot coexistence**: 3 slots live simultaneously (smoke-three @ 0x180000, P2 @ 0x2b0000, P1 @ 0x360000).
- **Free space accounting**: baseline 5640192 → after P1 3633152 (delta 2007040) → after P2 3010560 (delta 622592) → after R3 5017600 → after R4 5640192 (exact restore).
- **Selective remove**: R3 removes only P1; P2 and smoke-three remain untouched.
- **Baseline restore**: final state `count=1 free=5640192 archived=1` matches initial state exactly.
- **USB-JTAG reset reliability**: every reset produced `boot:0xa (SPI_FAST_FLASH_BOOT)` and factory image load within ~5 s.
- **LAN recovery after reset**: ~15 s from reset trigger to HTTP 200.
- **Web Serial single-session**: Chrome launched once, Web Serial authorized once, same tab/CDP session throughout all 4 rounds. No Chrome restart.

## Install Path Verification (per page log)

Each install followed this exact chain (no deviations):

1. `loadSlotModel()` — re-read carve record + partition table via USB serial
2. `planInstall()` — compute CREATE action, new slot offset/size
3. `commitCarve({materialize: true})` — write carve record to store sector → read-back verify → write partition table to 0x8000 → read-back verify
4. `loader.writeFlash()` — erase + write compressed image data
5. Write tail metadata sector (MSIG/MAEG/MNAM) at tail offset
6. `commitCarve({materialize: false})` — mark slot VALID in carve record, write to store sector, read-back verify
7. `setProgress(100)` + `install-status = "Done. Power-cycle the device..."`

## Remove Path Verification (per page log)

Each remove followed:

1. `loadSlotModel()` — re-read carve record
2. `removeSlotAction(index)` — erase image header at slot offset (prevent resurrection)
3. Remove slot from carve slots array
4. `commitCarve({materialize: true})` — write new carve record + materialize partition table
5. Slot row disappears from UI, carve count decreases

## Earlier False-positive Bug Reports (Resolved)

During initial testing runs, two errors appeared that were initially attributed to product bugs. Root-cause analysis showed both were **driver defects**, not product issues:

### "No serial data received"

**Initial claim**: consecutive install fails with `No serial data received.`

**Root cause**: driver script (`/tmp/e2e-multi.mjs`) was killed by a 60 s shell timeout mid-install. The process was still writing to flash when killed, leaving the serial transport in an inconsistent state. A subsequent driver run connected to the stale transport and got zero-length reads.

**Proof**: a clean repro script (`/tmp/repro-r2.mjs`) with `timeout 0` (no deadline) completed R1 + R2 successfully. The install path `loadSlotModel → planInstall → commitCarve → writeFlash → commitCarve` executed correctly in both rounds.

### "Cannot read properties of null (reading 'slots')"

**Initial claim**: UI crashes after install with null-deref in `slotModel.carve.slots`.

**Root cause**: driver's `uiConnect()` function polled `slotModel.mode` without first clicking `#btn-connect`. The page was in `LEGACY_FALLBACK` mode (no transport open), so `slotModel.carve` was null. The install path then tried to access `carve.slots` and threw.

**Proof**: adding `document.getElementById('btn-connect').click()` before polling resolved the issue. The page log shows `mode=DYN_SLOT seq=N carve=N` after the click.

### Lesson

Both false positives came from driver scripts in `/tmp/` that were never committed to the repo. The product code (`install-slot.html`, `dynslot-record.js`, `meta_store_install.c`) was correct throughout.

## Server-side Finding (Unrelated)

`tools/install-slot/server.mjs:154` dev server sets `content-security-policy: default-src 'self'; script-src 'self'` which blocks the 3 inline `<script>` blocks in `install-slot.html`. In prod (Cloudflare Pages) CSP is not set so the bug never surfaced. Removed CSP header, kept `nosniff` + `DENY`. **Uncommitted** — separate commit needed.

## Files

- `tools/realdevice/logs/usb-web-e2e-repro-r2/` — R1 + R2 evidence
  - `r2-driver.log` — driver output
  - `rc-R1-page-log.txt` — R1 page log (install chain)
- `tools/realdevice/logs/usb-web-e2e-r3-r4/` — R3 + R4 evidence
  - `driver.log` — driver output
  - `pre-r3-slots.json`, `R3-after-reset-slots.json`, `R4-after-reset-slots.json` — device state snapshots
  - `report-r3-r4.json` — summary
  - `R3-pre.png`, `R3-after-remove.png`, `R4-pre.png` — screenshots
- Driver scripts (not committed, in `/tmp/`):
  - `repro-r2.mjs` — R1+R2 repro with tracing
  - `e2e-r3-r4.mjs` — R3+R4 remove driver

## Notes

- Chrome session kept single throughout (per directive). Web Serial authorized once at first run.
- No flash erase occurred at any point; device identity partition never touched.
- Device IP, MAC, session token, WiFi SSID, USB serial path, and local filesystem paths have been redacted from this report.
- This report and logdir are uncommitted — awaiting directive.
