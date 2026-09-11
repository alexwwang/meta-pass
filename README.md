[简体中文](README.zh_CN.md) | English

# meta-pass — multi-firmware launcher for FoloToy AI Passport

meta-pass turns the AI Passport into a multi-firmware device: it lives in the factory
partition as a permanent launcher, imports other firmware images (built for this hardware)
into two local 2 MB flash slots over Wi-Fi, boots them without reflashing the device, and
offers an on-device menu to view, boot, and delete stored firmware.

Design document (single source of truth): [`docs/assets/meta-pass-design.md`](docs/assets/meta-pass-design.md).

## Features

- **Two firmware slots** (`ota_0@0x360000`, `ota_1@0x560000`, 2 MB each); the launcher
  itself stays in the protected baseline layout (`factory@0x10000`, `cardid@0x356000`
  untouched, gate-enforced).
- **Cable-free import**: the Import page starts a WPA2 SoftAP with a random password and a
  one-time 6-digit pairing code shown on the screen; upload the app `.bin` from any browser
  at `http://192.168.4.1/`. Uploads stream to flash in 1024-byte chunks.
- **Integrity checks**: image magic, chip id (ESP32-C3), slot size, full-image SHA-256
  (shown on screen for comparison), plus the authoritative `esp_ota_end()` verification.
  Unsigned firmware requires an extra-long-press (LONG2) confirmation before booting.
- **Anti-brick by default**: app rollback is enabled, so an un-adapted child firmware runs
  as a one-session trial — any reboot (crash, power cycle) returns to the launcher.
  Adapted children stay resident (see below).
- **Local management**: list slots with name/version/size/hash, boot or delete with
  LONG2-confirmed destructive actions.

## Buttons

| Key | Action |
| --- | --- |
| UP/DOWN click | move selection |
| OK click | enter / confirm selection / cancel on confirm pages |
| OK long-press (LONG, 1.5 s) | back |
| OK extra-long (LONG2, 3 s) | confirm boot/delete; inside an adapted child = return to launcher |

## Adapting a child firmware (optional)

Include `main/metapass_hook.h` in the child firmware:

1. Call `metapass_mark_valid()` after its self-check to persist across reboots
   (otherwise it behaves as a trial boot and returns to the launcher on the next reboot).
2. On `BSP_BTN_LONG2` of the OK key, call `metapass_return_to_launcher()`. Note that a
   LONG event fires first at 1.5 s; assign LONG a harmless in-app action (e.g. page back).


## USB serial install (no Wi-Fi, second channel)

Install child firmware over a data cable with Chrome — zero device firmware changes
(uses the ROM download mode):

```bash
node tools/install-slot/server.mjs   # open http://localhost:4191/
```

1. Power on the device **while holding UP** (GPIO0 low → ROM download mode), then plug in
   USB;
2. In the page: connect the serial port → pick a slot (Slot 0/1) → pick a source: a local
   `.bin` (Full Flash merged images are unpacked to their app image automatically) or a
   community play link (auto-downloaded and verified against the published SHA-256);
3. Click Install to write and verify, then power-cycle and boot from the meta-pass menu.

Design and tradeoffs: `docs/assets/meta-pass-design.md` §6.1. The unpacking logic has a
standalone test: `node tools/install-slot/test-extract.mjs`.

Un-adapted firmware still works: it simply returns to the launcher on every reboot, and
never needs reflashing — select it again from the menu.

## Build and flash

```bash
source <esp-idf-v5.5.3>/export.sh        # must be ESP-IDF v5.5.3
./tools/validate.sh --firmware           # build + protected-layout verification
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 build/FoloToy-AI-Passport-full.bin
```

Never run `idf.py erase-flash` on a device that already carries its identity
(`cardid` region). See `docs/development/engineering/protected-flash-layout.md`.

## Repository layout deltas vs upstream baseline

- `main/`: launcher UI (`main.c`), storage layer (`meta_store`), import channel
  (`meta_net`), pure logic (`meta_image`, `meta_slots`, `meta_import`), child hook
  (`metapass_hook.h`); demo pages removed.
- `components/bsp`: new `BSP_BTN_LONG2` button event (backward compatible).
- `partitions.csv`, `sdkconfig.defaults`: OTA slots + app rollback; BLE config removed
  (the launcher does not use it).
- `tests/` + `tools/validate.sh`: three new host-test suites wired into the static gate;
  `tools/install-slot/` is the USB serial install page (local service, see above).
- `docs/assets/`: fork-owned design documentation.
