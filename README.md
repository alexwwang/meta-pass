# meta-pass — turn the FoloToy AI Passport into a multi-firmware device

[简体中文](README.zh_CN.md) | English

meta-pass is a **multi-firmware launcher** for the FoloToy AI Passport (ESP32-C3, 8MB
flash): flash meta-pass once, then install community firmware into two 2MB slots at any
time and boot them from a menu — **no more full reflashes**. Plays no longer overwrite
each other, and the launcher is always one power cycle away.

<p align="center">
  <img src="docs/assets/images/meta-pass-launcher.png" alt="Launcher list: slots show Pocket Walkie / Passport Radar" width="200">
  &nbsp;
  <img src="docs/assets/images/meta-pass-slot-detail.png" alt="Slot detail: name, version, size, SHA-256" width="200">
  &nbsp;
  <img src="docs/assets/images/meta-pass-unsigned-warning.png" alt="LONG2 warning before booting unsigned firmware" width="200">
</p>

A stock Passport runs one firmware at a time; trying community plays means reflashing the
whole flash and back. meta-pass lives in the factory partition as a launcher and turns the
second half of the flash into two OTA slots for child firmware:

```text
0x000000  bootloader
0x008000  partition table   nvs / phy_init (identical to stock)
0x010000  factory (3MB)     ← the meta-pass launcher itself
0x310000  otadata           ← written by the launcher to pick a slot, then reboot
0x356000  cardid (16KB)     ← device identity; untouched by every channel
0x360000  ota_0 (2MB)       ← slot 0 (last 4KB = display-name blob)
0x560000  ota_1 (2MB)       ← slot 1 (last 4KB = display-name blob)
```

Selecting a slot = write otadata + reboot; the stock 2nd-stage bootloader does the switch.
No custom bootloader changes.

## Features

- **Dual-slot switching**: each slot row shows firmware name/version/size/SHA-256; pick
  and boot. The display name is written at install time (community firmware all carry the
  template's default `project_name`, so the real name can only come from the install
  channel).
- **Two install channels**:
  - **USB serial install page** (recommended): open a local page in Chrome, hold UP while
    powering on to enter ROM download mode, write straight into a slot; accepts a local
    `.bin` (Full Flash images are unpacked in-page) or a plays marketplace link
    (auto-downloaded and verified against the store's published SHA-256);
  - **Device hotspot + web import**: the device starts a SoftAP and shows a pairing code;
    upload from a phone or computer browser.
- **Integrity checks**: magic, chip id, size and segment structure, with `esp_ota_end()`
  as the authoritative recheck; the SHA-256 is shown on the detail page for comparison
  with the store's value.
- **Unsigned-firmware warning**: booting unsigned firmware requires an extra-long OK press
  (LONG2). Malicious firmware would still get full flash access (no eFuse-enforced
  signing) — only install firmware from sources you trust.
- **Never stuck in a child firmware**: un-adapted children are trial boots — any reboot
  (including power loss) returns to the launcher. Adapted firmware can persist and wires
  LONG2 to return to the launcher.
- **Identity safety**: the `cardid` partition is avoided by every install/flash path;
  `verify_firmware.py` byte-checks the baseline layout in the gate.

<p align="center">
  <img src="docs/assets/images/meta-pass-usb-installer.png" alt="USB serial install page: connect, pick slot, pick source, display name, progress and log" width="520">
</p>

## Quick start

### 1. Flash meta-pass (once)

Download `meta-pass_v0.1.bin` from Releases, or build it yourself (see
"Development"). Then:

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 meta-pass_v0.1.bin
```

The image ends at `0x312000` and never touches `cardid` (flashing tools only erase/write
the covered region; **never** run `erase-flash` on an identity-written device).

### 2. Install child firmware

**Option A: USB serial install page** (no hotspot needed):

```bash
node tools/install-slot/server.mjs   # open http://localhost:4191/
```

Hold UP while powering on → Connect in the page → pick a slot → choose a local file or
paste a plays link → Install → power-cycle. Full guide:
[tools/install-slot/README.md](tools/install-slot/README.md).

**Option B: device hotspot import** (no Chrome required):

Pick IMPORT FIRMWARE in the main list → the device starts a hotspot and shows a pairing
code → connect from a phone/computer, open `192.168.4.1` → enter the code, pick a slot,
upload a `.bin`.

### 3. Boot

UP/DOWN to pick a slot, OK for details, BOOT to confirm. Unsigned firmware asks for a
LONG2 (extra-long OK) confirmation.

## Button map

| Page | UP/DOWN | OK click | OK LONG2 (3 s) |
| --- | --- | --- | --- |
| Main list | select slot | open details / import page | — |
| Slot detail | BOOT/DELETE/BACK | confirm | DELETE needs LONG2 against accidents |
| Unsigned warning | — | cancel | confirm boot |
| Import page | — | — | exit import, back to list |

Inside an adapted child firmware: OK LONG2 = return to launcher (wired by the child, see
below).

## Adapting a child firmware (optional)

Children work unmodified (trial-boot mode). To persist across reboots and get LONG2
return, include `main/metapass_hook.h` and wire two calls:

1. Call `metapass_mark_valid()` after self-check (otherwise the next reboot returns to
   the launcher);
2. Route the OK key's LONG2 event to `metapass_return_to_launcher()`. Note the LONG event
   fires first at 1.5 s; give LONG a harmless in-app action (e.g. page back).

## Repository layout

| Path | Content |
| --- | --- |
| `main/` | Launcher UI (`main.c`), storage layer (`meta_store`), Wi-Fi import (`meta_net`), pure-logic modules (`meta_image`/`meta_slots`/`meta_import`/`meta_name`), child-firmware hook (`metapass_hook.h`) |
| `components/bsp/` | Board support package (stock + `BSP_BTN_LONG2` event) |
| `tools/install-slot/` | USB serial install page (local service + Web Serial page, zero dependencies) |
| `tools/validate.sh` | Unified gate: static checks + host tests + firmware build + protected-layout verification |
| `tests/` | Host tests (C, pure-logic modules, run on PC) |
| `docs/assets/meta-pass-design.md` | Design document (decision log and acceptance checklist) |

## Development

```bash
source <esp-idf-v5.5.3>/export.sh   # ESP-IDF v5.5.3 required
./tools/validate.sh --static        # repo checks + host tests
./tools/validate.sh --firmware      # firmware build + protected-layout verification
                                    # (isolated /tmp build, artifact copied back to
                                    #  build/meta-pass_v<version>.bin)
node tools/install-slot/test-extract.mjs   # installer unpacking / name-blob tests
```

Firmware changes are host-test-first (TDD); image parsing, slot metadata and checksums
are pure-logic modules with no ESP-IDF dependency.

## Verification record

| Category | Result (2026-09-11) |
| --- | --- |
| Build | Full `validate.sh` gate PASS; app 1,283,232 / 3,145,728 B; merged image 3,219,456 B, `cardid` untouched |
| Host tests | `meta_image`/`meta_slots`/`meta_import`/`meta_name` suites all pass; installer node tests 7/7 (vectors byte-locked against the C side) |
| Simulator (esp-emu) | Dual-slot merged image (plays 105 + 81): scan detection, real names, detail SHA-256 matches host computation, unsigned warning, LONG2 confirm, both slots boot, hard reset rolls back to launcher; Radar menu buttons work |
| Real device | USB install page Connect/write flow debugged step by step in real Chrome (see git log); full acceptance checklist pending on hardware, design doc §10 |
| Unverified | Full Wi-Fi import flow on hardware; Walkie buttons (no response even when flashed standalone in the simulator — suspected incompatibility with the simulator's ADC injection, to be confirmed on hardware) |

## Relationship to the official firmware

This repository is based on
[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) (`f75873f`, MIT): the
`factory`/`cardid` layout, `verify_firmware.py` and other baseline contracts remain
byte-compatible; the stock demo pages were removed to make room for the launcher UI.
This is an unofficial project, not affiliated with FoloToy.

## FAQ

| Symptom | Fix |
| --- | --- |
| Serial picker is empty | Device is not in download mode (hold UP while powering on), or the USB cable is charge-only |
| Child firmware ignores buttons / no LONG2 return | Un-adapted firmware has no return hook; power-cycle to return (rollback). By design |
| Rebooting a child lands back in the launcher | Un-adapted children are trial boots; persist requires `metapass_mark_valid()` in the child |
| Slot shows "AI-Passport" instead of the play name | The firmware was installed without a display-name blob (merged image / old channel); reinstall via the USB page with a Display name |
| Image rejected | Over 2044KB (the slot's last 4KB is reserved for the name blob), or not an ESP32-C3 image |
