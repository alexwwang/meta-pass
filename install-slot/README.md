# meta-pass USB Slot Installer

[简体中文](README.zh_CN.md) | English

Zero-dependency Chrome page that writes child firmware directly into a
meta-pass OTA slot over USB serial. Deployed via GitHub Pages for
community use; `tools/install-slot/server.mjs` is retained for local
development.

## Two deployment paths

### A. Cloudflare Worker (recommended, already deployed)

The page + API proxy are served together from one Cloudflare Worker:

```
https://meta-pass.springleeks.workers.dev/
```

The Worker serves the install page at `/` and proxies `/api/plays`,
`/api/play`, `/api/firmware` to `https://ai-passport.folotoy.cn` with
CORS headers added. To redeploy:

```bash
wrangler deploy worker/index.mjs
```

(CI auto-deploys on push to `main` when `worker/**` or `install-slot/**` changes.)

### B. Local Node server (no deployment needed)

For local development or when you prefer to run everything on your machine:

```bash
node tools/install-slot/server.mjs
# → install-slot server: http://localhost:4191/
```

Then open **http://localhost:4191/** in Chrome.

The local server is **required** if you need:
- Web Serial (must be served from a secure context — `localhost` qualifies;
  GitHub Pages' HTTPS also qualifies, so path A works too).
- To avoid any external proxy dependency.

Path A and B are functionally equivalent; the only difference is where the
API proxy lives.

## Requirements

- **Chrome or Edge** on a desktop (the Web Serial API; Safari/Firefox do not work);
- **Node.js** ≥ 18 (only for the local server, path B; no npm install, zero deps);
- a USB **data** cable (charge-only cables will not show the device).

## Usage

1. **Enter download mode**: power on the device **while holding UP** (UP shorts
   GPIO0 low, which is the ROM strapping pin), then plug in the USB cable.
2. **Connect**: click *Connect* and pick the serial port. The log should show
   `Connected: ESP32-C3`. A non-C3 chip only triggers a warning.
3. **Select slot**: Slot 0 (`0x180000`), Slot 1 (`0x360000`), or Slot 2
   (`0x560000`).
4. **Firmware source**:
   - *Local file*: an app `.bin`, or a Full Flash merged image — the page
     unpacks the app image in JS (partition table at `0x8000` + ESP image
     segment walk);
   - *Community play*: paste a play link such as
     `https://ai-passport.folotoy.cn/plays/105/` (or just `105`). The proxy
     fetches the download and the page verifies the SHA-256 against the value
     published by the store.
5. **Display name** (optional, ≤32 printable ASCII): pre-filled from the play
   title or file name; written into the slot's name blob sector and shown in
   the meta-pass menu.
6. **Install** → wait for `Done.` → **power-cycle** the device (unplug/replug
   or power button), then select the slot in the meta-pass menu. Unsigned
   firmware asks for an extra-long OK press (LONG2) before booting.

## Notes

- App images are limited to **partition size − 4KB** (the slot's last 4KB sector
  holds the name blob). Per-slot limits: slot 0 ≈ 1.88 MB, slot 1 = 2 MB,
  slot 2 ≈ 2.68 MB.
- Un-adapted child firmware has no way back to the launcher except a power
  cycle (rollback returns to meta-pass automatically). Adapted firmware wires
  `metapass_return_to_launcher()` to LONG2 — see
  `docs/assets/meta-pass-design.md` §5.
- The page never touches `factory`, `cardid`, or `otadata` — only the three
  slot offsets.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Serial picker is empty | Device not in download mode (hold UP while powering on), or a charge-only USB cable |
| `esptool-js still loading…` | Vendor bundle still loading; wait a second and retry (fully local, no CDN) |
| `SHA-256 mismatch` | Corrupted/tampered download; do not install, retry |
| Page buttons all dead | Hard-refresh with Cmd/Ctrl+Shift+R (stale cached page) |
| Play fetch returns 502 | Proxy is down — if using GitHub Pages, confirm the Cloudflare worker is deployed and healthy |

## Development

- `extract-app-image.js`, `name-blob.js`: pure ES modules shared by page and
  Node tests.
- `vendor/`: esptool-js 0.5.6 + deps (pako, atob-lite, ESP32-C3 target and
  stub flasher), localized from the jsDelivr `+esm` build with import paths
  rewritten — the page makes zero external requests except the API proxy.
- Tests: `node tools/install-slot/test-extract.mjs` (image unpacking, name
  blob vectors byte-locked against `tests/test_meta_name.c`, size-limit
  boundaries).

## Repo layout

```
install-slot/                 # static assets (served by Worker at /)
  install-slot.html           # the page
  extract-app-image.js        # image unpacker (ES module)
  name-blob.js                # name blob packer/unpacker (ES module)
  vendor/                     # bundled esptool-js + deps

worker/
  index.mjs                   # Cloudflare Worker (page + API proxy)

tools/install-slot/           # original dev dir (server.mjs kept for local)
  server.mjs                  # local Node HTTP proxy (alternative to Worker)
  …same files as above…
```
