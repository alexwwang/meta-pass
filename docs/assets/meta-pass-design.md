# meta-pass Design Document

[简体中文](meta-pass-design.zh_CN.md) | English

> Single source of truth. Read this before modifying meta-pass code; record design changes
> in "Decision Log" first.

## 1. Goal and Scope

meta-pass is a **multi-firmware launcher** for the AI Passport. It lives in the factory
partition, imports firmware images adapted to this hardware into local flash slots, boots
them without reflashing the whole flash, and provides a local management UI to list, boot,
and delete stored firmware.

Non-goals: no OTA cloud service; no mandatory firmware signing (see §7); no bootloader
changes in v1.

## 2. Terms

- **Launcher**: meta-pass itself, flashed in the factory partition.
- **Child firmware**: a third-party/derivative application image (single app `.bin`)
  written into an OTA slot.
- **Adapted child**: a child firmware that includes the meta-pass adaptation hook (§5).
- **Trial boot**: semantics for un-adapted children — any reboot returns to the launcher.

## 3. Flash Layout

The baseline contract (enforced by `tools/verify_firmware.py`) is preserved byte-for-byte:
`factory@0x10000/3MB`, `cardid@0x356000/0x4000`. New partitions only use the gap after
factory and free space after cardid:

| Partition | Type | Offset | Size | Notes |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 0x6000 | unchanged (children share this NVS namespace) |
| phy_init | data/phy | 0xf000 | 0x1000 | unchanged |
| factory | app/factory | 0x10000 | 0x300000 | **unchanged**, meta-pass itself |
| otadata | data/ota | 0x310000 | 0x2000 | new; all-0xFF means boot factory |
| cardid | data/nvs | 0x356000 | 0x4000 | **unchanged**, protected identity region |
| ota_0 | app/ota_0 | 0x360000 | 0x200000 | new slot 0 (app partitions need 64KB alignment; 0x35A000 is not aligned, so start at 0x360000) |
| ota_1 | app/ota_1 | 0x560000 | 0x200000 | new slot 1; 0x760000–0x800000 (640KB) left spare |

Constraints: child image ≤ 2044KB (the slot's last 4KB sector is reserved for the display-name blob, §6.2); cardid region in the merged image must be all 0xFF; the
project name stays `FoloToy-AI-Passport` (the gate hardcodes the image file name).

## 4. Boot and Rollback Model

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`:

- **Boot a child**: after validation, the launcher calls
  `esp_ota_set_boot_partition(ota_x)` + `esp_restart()`.
- **Adapted child**: calls `esp_ota_mark_app_valid_cancel_rollback()` after self-check →
  persistent across reboots.
- **Un-adapted child**: never self-validates → any reboot (crash/power loss/watchdog)
  makes the bootloader fall back to factory automatically. "Runs once, returns to the
  launcher on reboot." Anti-brick needs no third-party cooperation.
- **Launcher itself**: calls `esp_ota_mark_app_valid_cancel_rollback()` early in
  `app_main` so the rollback target is always valid.

## 5. Child Adaptation Convention (optional but recommended)

Children are separately built derivatives of this repository. Adapt for full experience:

1. Include `main/metapass_hook.h`; on `BSP_BTN_LONG2` (OK key, 2x long-press) call
   `metapass_return_to_launcher()` (set boot partition to factory and restart).
2. Call `esp_ota_mark_app_valid_cancel_rollback()` after self-check to stay persistent.
3. When using the shared NVS, prefix your namespaces to avoid clashing with other
   firmware.

Button model (hardware ruling): the three buttons share one ADC divider node on GPIO0, so
combos collapse to the dominant single button (UP+anything=UP; DOWN+OK reads ~212mV, inside
the DOWN window) — **combos are unusable**; the power key is hardware power control and not
readable by firmware. Therefore the return mechanism uses **two-tier long-press**: `LONG`
(default duration) = in-app back, `LONG2` (2x default) = return to launcher. The BSP gains
a `BSP_BTN_LONG2` event, backward compatible (`LONG` semantics unchanged).

## 6. Import Channel and Protocol

Wi-Fi **SoftAP (AP-only)** + local web upload. Trust anchor = physical possession (being
able to see the screen):

1. User enters the Import page on the launcher → device starts a WPA2 SoftAP, SSID
   `metapass-XXXX`; a random password and a **6-digit one-time pairing code** are shown on
   screen; the session auto-closes after N minutes (default 5) of inactivity.
2. The uploader joins the AP, opens `http://192.168.4.1/`, enters the pairing code, picks
   a slot, and uploads a `.bin`.
3. HTTP constraints (per the SoftAP resource-budget experience): reject oversized
   Content-Length immediately; stream in 1024-byte chunks (`esp_ota_begin/write` straight
   to the slot, never buffering the whole image in RAM); handle timeout/disconnect in the
   receive loop; any failure path erases the slot and marks it invalid.
   `max_connection=1`.
4. After the write completes, validate (§7); on success mark the slot bootable, on failure
   erase it.

Minimal HTTP API: `GET /` (page), `POST /api/session` (pairing code → session),
`POST /api/upload?slot=N` (body = firmware, session required), `GET /api/status`.

Resource budget: record free heap and largest free block before entering Import; keep
heavy resources (audio) uninitialized; fully stop and release Wi-Fi/HTTP on page exit
(mirroring demo_wifi enter/exit).

### 6.1 USB Serial Install Channel (Route A, alongside Wi-Fi import)

A cable-based second channel: **USB data cable + Chrome browser**. Zero firmware changes,
built on the ROM bootloader:

1. Hold **UP** while powering on/resetting: UP pulls GPIO0 (a strapping pin) to ground via
   0Ω → the chip enters ROM download mode.
2. Open the `tools/install-slot/` page in desktop Chrome (served on localhost; Web Serial
   requires a secure context, which the device-side `http://192.168.4.1` cannot provide,
   so the page lives on the computer).
3. The page uses esptool-js over USB Serial/JTAG to write the child firmware to a slot
   offset (`0x360000`/`0x560000`), with automatic post-write verification; after a reset,
   meta-pass scans and can boot it.

Two firmware sources:

- **Local `.bin`**: an app image is written as-is; a Full Flash merged image is unpacked
  in JS (parse the partition table at `0x8000` to locate the factory app, then walk the
  ESP image segment table for the exact length).
- **Community play link**: the plays API sends no CORS headers, so the local server
  (`server.mjs`) proxies the download (same pattern as passport-sim's community-import,
  restricted to `ai-passport.folotoy.cn`). The play detail API provides `firmwareSha256`;
  the page verifies the hash after download — closing the loop with the hash meta-pass
  shows during its boot scan.

Boundaries: writes only the two slot offsets; never touches factory/cardid/otadata;
app images > 2MB are rejected. Not covered: a BLE channel (slow, needs a custom chunking
protocol, requires HTTPS-hosted entry, cannot be verified in the simulator — dropped,
see §11).

### 6.2 Slot Display-Name Blob

A firmware's real name (e.g. "Pocket Walkie") exists only in the store metadata; the
image's `project_name` is usually the build-template default (community firmware all say
`FoloToy-AI-Passport`), so the real name cannot be recovered at scan time. The display
name is therefore written **at install time** into the slot partition's last 4KB sector
(`slot_offset + 0x1FF000`):

- blob format: `magic "MNAM"` (4B) + `name_len` (1B, 1–32, aligned with the slot registry field) + name (printable ASCII) + XOR checksum (1B);
- launcher scan: valid blob → show the real name; otherwise fall back to the core name
  (`project_name` minus the `FoloToy-` prefix);
- name source: USB install page = community play's English title / local file name;
  Wi-Fi import page = optional text input;
- the app image limit shrinks to 2044KB accordingly; deleting a slot erases the whole
  partition including the blob.

## 7. Firmware Validation Policy

Mandatory (every child):

- Image header magic `0xE9`, chip id = ESP32-C3, size ≤ 2044KB (slot tail reserved for the display-name blob), sane segment count;
- Compute the full-image SHA-256 and show it on the confirm page (manual comparison
  against the publisher's hash).

Optional badge: if the package carries a signature, verify it and show "Signed"; not
mandatory — existing market firmware cannot be forced to re-adapt. Unsigned firmware shows
a warning page requiring a **LONG2** confirm before boot.

Honest boundary: once booted, an unsigned child has full flash access; software cannot
stop a malicious child from erasing cardid. Trust comes from user judgment + pairing-code
physical possession + trial-boot isolation. eFuse write protection / Secure Boot v2
(irreversible) is deferred for separate evaluation.

## 8. Local Management UI

Keeps the `ui_pixel` theme (sky/grass/title board/mascot) and the top-right battery
indicator (avoiding the cloud at `x≈188,y≈8`). UI text in English.

- **Main list**: slot 0/1 rows show empty / the display name (real name written at install
  time, core-name fallback otherwise); UP/DOWN to select, OK click for details.
- **Detail page**: Boot (unsigned requires warning page LONG2 confirm), Delete (confirm
  page LONG2), back.
- **Import page**: shows SSID/password/pairing code/IP/countdown; OK LONG exits and fully
  releases the network stack.
- Global: `OK LONG` = back; `OK LONG2` inside a child = return to launcher (inside the
  launcher, same as LONG).

Delete = `esp_partition_erase_range` on the whole slot + clear metadata; it does not touch
child-owned NVS data (children manage their own namespaces).

## 9. Test Strategy (TDD)

Pure logic decoupled from ESP-IDF/LVGL comes first, covered by host tests:

- `meta_image`: image header/size/chip-id/segment validation (valid, bad magic, wrong
  chip, oversize, truncated);
- `meta_slots`: slot registry and state transitions (empty/occupied/bootable/invalid);
- `meta_import`: import state machine (idle→ap→paired→receiving→verifying→done|error),
  pairing-code generation and comparison, Content-Length cap policy.

New tests are wired into `tools/validate.sh --static`. Hardware-dependent paths (flash
write, boot, rollback) go on the device-acceptance list.

## 10. Acceptance Criteria

- `./tools/validate.sh` fully green (static + firmware gates, including new host tests);
- Partition table: factory/cardid byte-identical to baseline; otadata/ota_0/ota_1
  non-overlapping; cardid all 0xFF;
- Device checklist (verify item by item at delivery): import one firmware and boot it;
  power-cycle auto-returns to launcher; adapted firmware persists; slot shows empty after
  delete; corrupt file rejected; wrong pairing code rejected; repeated Import enter/exit
  leaks nothing.

### 10.1 Simulator End-to-End Verification (2026-09-11, local esp-emu instance)

Route A's serial transport itself cannot be verified in the simulator (Web Serial only
enumerates real devices; the emulator does not run the mask-ROM download mode), but the
"installed" state can be constructed byte-for-byte: the app image of community play 105
(Pocket Walkie), unpacked by `tools/install-slot/extract-app-image.js` (Full image SHA-256
matched the published value), was merged at `ota_0@0x360000` via esptool `merge_bin` and
uploaded to the simulator. The full chain passed: launcher scan detected the slot (size
1262 KB, SHA-256 `bf98f879…` identical to the host-side computation) → detail page →
unsigned-firmware warning → LONG2 confirm → child firmware booted and ran (WALKIE UI) →
hard reset returned to the launcher per the rollback model (un-adapted child = trial boot).
Not covered: the serial transport itself, Wi-Fi import (simulator has no AP support).

2026-09-11 round 2 (dual slots + display-name blob): a merged image carrying
play 105@ota_0 ("Pocket Walkie" blob) and play 81@ota_1 ("Passport Radar" blob).
The slot list shows the real names; both slots boot. Radar (official firmware, stock BSP)
navigates its menu with injected buttons — button input under meta-pass works fine.
Two known simulator boundaries: Walkie (community firmware) ignores buttons even when
flashed standalone (not meta-pass's doing; presumably its button-reading path is
incompatible with the simulator's ADC injection — verify on hardware); Radar's main
feature needs BLE, and the simulator halts on BLE activity (no BLE support).

## 11. Decision Log

| Date | Decision | Alternatives | Rationale |
| --- | --- | --- | --- |
| 2026-09-10 | Branch feature/meta-pass from main | develop on main | repo rule: main stays the upstream baseline |
| 2026-09-10 | 2 slots x 2MB | 3 slots x 1.5MB | baseline firmware is 1.48MB; 1.5MB leaves no headroom |
| 2026-09-10 | SoftAP + web upload | USB serial transfer | cable-free; repo has SoftAP budget experience |
| 2026-09-10 | On-screen one-time pairing code | fixed password / 2FA | physical possession as trust anchor; simple UX |
| 2026-09-10 | Mandatory integrity + optional signature | mandatory signing / eFuse SBv2 | cannot force existing market firmware to re-adapt; eFuse is irreversible |
| 2026-09-10 | Rollback on; un-adapted = trial boot | require child mark_valid | no leverage over third parties; crashes auto-return to launcher |
| 2026-09-10 | LONG2 two-tier long-press return | combo key on+ok | single-ADC-node combos are physically indistinguishable |
| 2026-09-10 | Keep stock bootloader | custom bootloader recovery button | GPIO0 is a strapping pin; ADC pull-up cannot do power-on recovery; too risky |
| 2026-09-11 | Add USB serial install channel (Route A) | in-app custom serial protocol (Route B) | zero firmware changes; mature ROM bootloader + esptool verification; UP key is a natural download-mode trigger |
| 2026-09-11 | Installer page on computer localhost | device-served / public hosting | Web Serial needs a secure context; plays API has no CORS headers, needs a local proxy |
| 2026-09-11 | Community links + JS unpacking of full images | app-image-only input | community ships full images only; unpacking is deterministic; plays API ships firmwareSha256 for a closed loop |
| 2026-09-11 | Drop BLE import channel | BLE GATT chunked transfer | slow (minutes for 2MB), custom protocol needed, entry must be HTTPS-hosted, not verifiable in the simulator |
| 2026-09-11 | Display name as a 4KB blob at slot tail | NVS storage; built-in play list | the USB page in ROM download mode can only write raw flash, not NVS structures; a built-in list goes stale with every new play |
| 2026-09-11 | Vendor esptool-js locally | jsdelivr CDN dynamic import | a slow/unreachable CDN wedged the whole page behind a top-level await (hit on first real-device attempt); 3 local files, 81KB, zero external requests; also fixes name-blob.js missing from the static whitelist, which broke page module loading entirely |
