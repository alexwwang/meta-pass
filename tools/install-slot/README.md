# meta-pass USB Slot Installer

[简体中文](README.zh_CN.md) | English

A localhost web page that writes child firmware directly into a meta-pass OTA slot over
USB serial — no Wi-Fi import needed and **zero device-side firmware requirements** (it
uses the ESP32-C3 ROM download mode).

## Requirements

- **Chrome or Edge** on a desktop (the Web Serial API; Safari/Firefox do not work);
- **Node.js** ≥ 18 (only for the tiny local server; no npm install, zero dependencies);
- a USB **data** cable (charge-only cables will not show the device).

## Start the local service

From the repository root:

```bash
node tools/install-slot/server.mjs
# → install-slot server: http://localhost:4191/
```

Then open **http://localhost:4191/** in Chrome. `PORT=8080 node tools/install-slot/server.mjs`
changes the port.

The page must be served from `localhost` — Web Serial requires a secure context, and the
device's own `http://192.168.4.1` page cannot provide one. That is why this tool exists as
a local service instead of being hosted on the device.

## Usage

1. **Enter download mode**: power on the device **while holding UP** (UP shorts GPIO0 low,
   which is the ROM strapping pin), then plug in the USB cable.
2. **Connect**: click *Connect* and pick the serial port. The log should show
   `Connected: ESP32-C3`. A non-C3 chip only triggers a warning.
3. **Select slot**: Slot 0 (`0x360000`) or Slot 1 (`0x560000`).
4. **Firmware source**:
   - *Local file*: an app `.bin`, or a Full Flash merged image — the page unpacks the app
     image in JS (partition table at `0x8000` + ESP image segment walk);
   - *Community play*: paste a play link such as
     `https://ai-passport.folotoy.cn/plays/105/` (or just `105`). The local server proxies
     the download (only `ai-passport.folotoy.cn` is allowed) and the page verifies the
     SHA-256 against the value published by the store.
5. **Display name** (optional, ≤32 printable ASCII): pre-filled from the play title or file
   name; written into the slot's name blob sector and shown in the meta-pass menu.
6. **Install** → wait for `Done.` → **power-cycle** the device (unplug/replug or power
   button), then select the slot in the meta-pass menu. Unsigned firmware asks for an
   extra-long OK press (LONG2) before booting.

## Notes

- App images are limited to **2044KB** (the slot's last 4KB sector holds the name blob).
- Un-adapted child firmware has no way back to the launcher except a power cycle
  (rollback returns to meta-pass automatically). Adapted firmware wires
  `metapass_return_to_launcher()` to LONG2 — see `docs/assets/meta-pass-design.md` §5.
- The page never touches `factory`, `cardid`, or `otadata` — only the two slot offsets.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Serial picker is empty | Device not in download mode (hold UP while powering on), or a charge-only USB cable |
| `esptool-js still loading…` | Vendor bundle still loading; wait a second and retry (fully local, no CDN) |
| `SHA-256 mismatch` | Corrupted/tampered download; do not install, retry |
| Page buttons all dead | Hard-refresh with Cmd/Ctrl+Shift+R (stale cached page) |

## Development

- `extract-app-image.js`, `name-blob.js`: pure ES modules shared by page and Node tests.
- `vendor/`: esptool-js 0.5.6 + deps (pako, atob-lite, ESP32-C3 target and stub flasher),
  localized from the jsDelivr `+esm` build with import paths rewritten — the page makes
  zero external requests.
- Tests: `node tools/install-slot/test-extract.mjs` (image unpacking, name blob vectors
  byte-locked against `tests/test_meta_name.c`, size-limit boundaries).
