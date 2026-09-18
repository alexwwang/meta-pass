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

3-Slot architecture (feat/shrink): factory shrunk to 1.44MB; cardid-before gap
is reused as ota_0; otadata moved to flash tail. The protected `cardid@0x356000/0x4000`
is unchanged and enforced by `tools/verify_firmware.py`:

| Partition | Type | Offset | Size | Notes |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 0x6000 | unchanged (children share this NVS namespace) |
| phy_init | data/phy | 0xf000 | 0x1000 | unchanged |
| factory | app/factory | 0x10000 | **0x170000** | shrunk from 3MB; meta-pass launcher, target < 1.43MB |
| ota_0 | app/ota_0 | **0x180000** | **0x1D6000** (1.84MB) | new; reuses cardid-before gap |
| cardid | data/nvs | 0x356000 | 0x4000 | **unchanged**, protected identity region |
| ota_1 | app/ota_1 | 0x360000 | 0x200000 (2MB) | unchanged (app partitions need 64KB alignment; 0x35A000 is not aligned) |
| ota_2 | app/ota_2 | **0x560000** | **0x29E000** (2.61MB) | new; dual-use as child firmware slot or audio storage (see §6.3) |
| otadata | data/ota | **0x7FE000** | 0x2000 | moved from 0x310000 to flash tail; all-0xFF means boot factory |

Constraints: child image ≤ (partition size − 4KB) (the slot's last 4KB sector is reserved
for the display-name blob, §6.2); cardid region in the merged image must be all 0xFF; the
project name stays `FoloToy-AI-Passport` (the gate hardcodes the image file name).

### 6.3 ota_2 Dual-Use Storage

ota_2 serves two roles depending on runtime state:

- **Child firmware slot**: a third-party firmware image can be written to ota_2 and
  booted by the launcher via `esp_ota_set_boot_partition()`.
- **Audio storage**: a recording child firmware (running from ota_0 or ota_1) can mount
  littlefs on ota_2 for audio file storage. The child first checks `esp_image_verify()`
  on ota_2: if a valid image is found, the partition is not touched; if empty/invalid,
  the child erases and mounts it as a filesystem.

The partition type remains `app` (subtype `ota_2`), so the bootloader can still select
it as a boot target. Littlefs mounting ignores partition type — `esp_littlefs_mount()`
locates the partition by label regardless of kind. This dual-use is a runtime convention,
not enforced by the partition table.


## 4. Boot and Rollback Model

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + **single-session model (2026-09-17)**:

- **Boot a child**: after validation, the launcher calls
  `esp_ota_set_boot_partition(ota_x)` + `esp_restart()`.
- **Every power-on returns to the launcher**: the child runs for the current session only.
  On any reboot/power-cycle the bootloader falls back to factory automatically — the ota
  state is never promoted to VALID (the hook no longer calls `cancel_rollback`), and the
  launcher maintains the invariant "launcher ran ⇒ otadata is empty ⇒ next boot defaults
  to factory" by erasing otadata early in `app_main`. Boundary note: a device already held
  by a pre-model child (VALID in otadata) never reaches the launcher — unlock via that
  child's return hook (OK long-press) or a re-flash. Crash/power-loss auto-recovery is the
  same mechanism (anti-brick).
- **Launcher itself**: `app_main` erases otadata early so the bootloader defaults to
  factory; nothing else needed (factory is the default boot target when otadata is empty).

## 5. Child Adaptation Convention (optional but recommended)

Children are separately built derivatives of this repository. Adapt for full experience:

1. Include `main/metapass_hook.h`; on `BSP_BTN_LONG` (OK key, long-press 1.5s) call
   `metapass_return_to_launcher()` (set boot partition to factory and restart).
2. Optionally call `metapass_mark_valid()` after self-check as a signature self-diagnostic
   (return value only; it does not change boot behavior — children are single-session).
3. When using the shared NVS, prefix your namespaces to avoid clashing with other
   firmware.

Button model: the OK key has a single long-press threshold (~1.5s). In meta-pass:
`LONG` = back to previous page; in confirmation pages: `LONG` = confirm dangerous action.
The power key is hardware power control and not readable by firmware. To switch child
firmware, power-cycle the device (press power key to shut down, then power on again).

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
   offset (`0x180000`/`0x360000`/`0x560000`), with automatic post-write verification;
   after a reset, meta-pass scans and can boot it.

Two firmware sources:

- **Local `.bin`**: an app image is written as-is; a Full Flash merged image is unpacked
  in JS (parse the partition table at `0x8000` to locate the factory app, then walk the
  ESP image segment table for the exact length).
- **Community play link**: the plays API sends no CORS headers, so the local server
  (`server.mjs`) proxies the download (same pattern as passport-sim's community-import,
  restricted to `ai-passport.folotoy.cn`). The play detail API provides `firmwareSha256`;
  the page verifies the hash after download — closing the loop with the hash meta-pass
  shows during its boot scan.

Boundaries: writes only the three slot offsets; never touches factory/cardid/otadata;
app images larger than (slot_size − 4KB) are rejected. Not covered: a BLE channel (slow,
needs a custom chunking protocol, requires HTTPS-hosted entry, cannot be verified in the
simulator — dropped, see §11).

### 6.2 Slot Display-Name Blob

A firmware's real name (e.g. "Pocket Walkie") exists only in the store metadata; the
image's `project_name` is usually the build-template default (community firmware all say
`FoloToy-AI-Passport`), so the real name cannot be recovered at scan time. The display
name is therefore written **at install time** into the slot partition's last 4KB sector.
Because slot sizes differ (ota_0=0x1D6000, ota_1=0x200000, ota_2=0x29E000), the blob
offset is computed dynamically as `partition_size − 0x1000` rather than a fixed constant:

- blob format: `magic "MNAM"` (4B) + `name_len` (1B, 1–32, aligned with the slot registry field) + name (printable ASCII) + XOR checksum (1B);
- launcher scan: valid blob → show the real name; otherwise fall back to the core name
  (`project_name` minus the `FoloToy-` prefix);
- name source: USB install page = community play's English title / local file name;
  Wi-Fi import page = optional text input;
- the app image limit shrinks to (partition_size − 4KB) accordingly; deleting a slot
  erases the whole partition including the blob.

## 7. Firmware Validation Policy

Mandatory (every child):

- Image header magic `0xE9`, chip id = ESP32-C3, size ≤ (slot_size − 4KB) (a single 4KB
  tail metadata sector after the image carries MSIG/MAEG/MNAM; slot sizes vary:
  0x1D6000/0x200000/0x29E000), sane segment count;
- Compute the full-image SHA-256 and show it on the confirm page (manual comparison
  against the publisher's hash).

Signature badge (application-layer, reversible — no eFuse, no Secure Boot v2):

A child firmware may carry an ECDSA-P256 signature badge in the partition tail metadata
sector, immediately after `image_len`. The signature is verified against a public key
compiled into meta-pass (`main/meta_sign_pubkey.h`, generated from
`tools/signing/public.pem`). Layout inside the OTA partition:

```
[app image (image_len bytes)] [tail metadata sector (4KB)]
                              ↑
  esp_image_verify only checks image_len; the tail sector is safe to append.
  Tail sector layout:
    [0..127]       MSIG reserve
    [128..4055]    MAEG easter egg window
    [4056..4095]   MNAM display-name reserve
  MSIG format (variable length, max 81 bytes):
    [4B "MSIG"] [4B payload_len LE] [70..72B ECDSA-P256 DER signature]
    [1B xor checksum of preceding header+signature bytes]
```

`scan_one` calls `meta_sign_verify()` after `esp_image_verify()` passes. A slot with a
valid signature shows "SIGNED" on the detail page and boots directly on OK click (no
warning page). A slot without a signature badge is treated as unsigned — it goes
through the unsigned-firmware warning page with BOOT / CANCEL selection.

### 7.1 Optional Easter Egg Metadata

An optional `MAEG` text note occupies the middle of the same tail metadata sector, between
MSIG and MNAM. The field is **fixed-length (3928 bytes)**: text shorter than 3919 bytes is
0xFF-padded, so the MNAM window always sits at the sector's last 40 bytes regardless of egg
length. It is metadata only: it is not part of the signed digest, not covered by
ECDSA, and does not change trust semantics. Device code should treat missing/bad egg data
as absent.

```
  tail metadata sector offset 128 (fixed 3928-byte field):
  [4B "MAEG"] [4B payload_len LE] [3919B text area: printable ASCII + 0xFF padding]
  [1B xor checksum at fixed offset 3927, over all 3927 preceding bytes incl. padding]
```

On device, the egg text of a slot is viewable from its detail page via a hidden key
sequence: fast `UP UP DOWN DOWN` (four CLICKs, inter-key gap <0.5s) opens the egg page;
`UP/DOWN` scroll the text, `OK` click returns. Slots without a valid MAEG field show
"No egg."; corrupted fields show "Egg data corrupted.".

Signing a child firmware: `tools/signing/sign-firmware.sh <app.bin> [--egg-text "..."]`
appends the tail metadata sector. The ECDSA-P256 private key lives in the macOS Keychain
(tag `com.folotoy.meta-pass.signing`, created by `tools/signing/bin/keychain-keygen`);
the script signs via `bin/keychain-sign` and never touches key material on disk. The
public key is published at `tools/signing/public.pem` and embedded into
`main/meta_sign_pubkey.h` + `main/metapass_hook.h` by `tools/signing/gen-pubkey.py`.

Honest boundary: once booted, an unsigned child has full flash access; software cannot
stop a malicious child from erasing cardid. The signature badge proves firmware origin
(signed by the meta-pass key holder) but does not enforce boot-time blocking at the
hardware level. Trust comes from user judgment + pairing-code physical possession +
trial-boot isolation. eFuse write protection / Secure Boot v2 (irreversible) is deferred
for separate evaluation.

## 7.2 Launcher Upgrade Contract (data-preserving)

The partition layout is stable; launcher upgrades must never touch user data.
Binding rules:

- **Allowed write set** (the only regions an in-place upgrade may write):
  `bootloader@0x0`, `partition-table@0x8000`, factory app `@0x10000`, and
  `otadata@0x7FE000` written in its erased state (boot back to factory).
- **Never written**: `nvs@0x9000` (stored data: Wi-Fi credentials, per-app state), `cardid@0x356000`,
  `ota_0/1/2` (installed child firmware). These survive every upgrade.
- **Layout gate**: before writing, the installer reads back the device
  partition table (4 KB at `0x8000`) and byte-compares it with the upgrade
  bundle's `partition-table.bin`. Any difference refuses the upgrade — the
  "only factory moves" assumption no longer holds.
- **Artifact gate**: `tools/verify_firmware.py` requires the merged 8 MB image
  to keep `nvs`/`ota_0`/`ota_1`/`ota_2`/`otadata` fully erased (0xFF). A full
  image is for factory flashing only — esptool erases every sector it writes,
  so flashing it over an existing installation would destroy user data.
- **Tooling**: `tools/build-firmware.sh` emits ONE release artifact —
  `meta-pass_<version>.bin` (~1.1 MB), a **hybrid single file** that serves both
  channels: a bootable body (bootloader + partition table + phy + app laid out
  at their flash offsets, byte-identical to the head of the 8 MB merged image)
  plus a 44-byte `MPUPV2` footer (magic + body length u32le + body SHA-256).
  - Market install: flash tools write it raw at 0x0; the ROM boots the body and
    never reads the footer (it lands in unused tail space of the factory
    partition).
  - USB-installer upgrade ("7. Upgrade launcher"): the page verifies the
    footer, slices bootloader / partition table / app out of the body, treats
    otadata as a generated all-0xFF segment, then runs the existing read-back
    gate and minimal write set. Legacy `MPUPV1` upgrade containers (already
    distributed) remain accepted via `parseUpgradeArtifact`.
  - The 8 MB merged image stays in `build/` for `verify_firmware.py` and the
    QEMU harness only; it is not a release artifact. Old
    `meta-pass-bootable_*` / `meta-pass-upgrade_*` artifacts are stale and are
    rejected by the verifier.

## 8. Local Management UI

Keeps the `ui_pixel` theme (sky/grass/title board/mascot) and the top-right battery
indicator (avoiding the cloud at `x≈188,y≈8`). UI text in English.

- **Main list**: slot 0/1/2 rows show empty / the display name (real name written at install
  time, core-name fallback otherwise); UP/DOWN to select, OK click for details.
- **Detail page**: Boot (unsigned firmware goes to the warning page), Delete (confirm
  page, OK LONG to confirm), back.
- **Unsigned warning page**: warning text + BOOT / CANCEL rows; UP/DOWN to select, OK
  click to confirm; defaults to CANCEL; OK LONG cancels (back to detail).
- **Import page**: shows SSID/password/pairing code/IP/countdown; OK LONG exits and fully
  releases the network stack.
- Global: `OK LONG` = back; inside a child firmware `OK LONG` = return to launcher.

Delete = `esp_partition_erase_range` on the whole slot + clear metadata; it does not touch
child-owned NVS data (children manage their own namespaces).

## 9. Test Strategy (TDD)

Pure logic decoupled from ESP-IDF/LVGL comes first, covered by host tests:

- `meta_image`: image header/size/chip-id/segment validation (valid, bad magic, wrong
  chip, oversize, truncated);
- `meta_slots`: slot registry and state transitions (empty/occupied/bootable/invalid);- `meta_import`: import state machine (idle→ap→paired→receiving→verifying→done|error),
  pairing-code generation and comparison, Content-Length cap policy.

New tests are wired into `tools/validate.sh --static`. Hardware-dependent paths (flash
write, boot, rollback) go on the device-acceptance list.

## 10. Acceptance Criteria

- `./tools/validate.sh` fully green (static + firmware gates, including new host tests);
- Partition table: factory/cardid byte-identical to baseline; ota_0/ota_1/ota_2/otadata
  non-overlapping; cardid all 0xFF;
- **feat/shrink**: factory binary size < 1.43MB (target); 3 OTA slots visible in launcher
  list; blob offset dynamically computed per slot size;
- Device checklist (verify item by item at delivery): import one firmware into each slot
  and boot it; power-cycle auto-returns to launcher; adapted firmware persists; slot shows
  empty after delete; corrupt file rejected; wrong pairing code rejected; repeated Import
  enter/exit leaks nothing.

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

### 10.2 3-Slot Shrink Verification (2026-09-12, Web emulator)
`feat/shrink` built from a fresh `sdkconfig.defaults` at **1,024,608 bytes (1001 KB)**
against the 1.44 MB (`0x170000`) factory partition — 32.0% headroom.

The decisive check is the **dynamic display-name blob offset** (`part_size − 4 KB`),
since the old fixed `0x1FF000` would put ota_0's blob at `0x37F000`, i.e. inside the
ota_1 partition. A merged 8MB image was preloaded into the Web emulator carrying
all three slots:

| Slot | Partition | Blob offset | OCR result |
| --- | --- | --- | --- |
| ota_0 | 0x180000 / 0x1D6000 | 0x355000 | `SLOT 0: Pocket Walkie` |
| ota_1 | 0x360000 / 0x200000 | 0x55F000 | `SLOT 1: Passport Radar` |
| ota_2 | 0x560000 / 0x29E000 | 0x7FD000 | `SLOT 2: Walkie Clone` |

All three names were read back from their own slot tails, which only holds if each
offset is derived from that slot's own size. `ota_0` boots correctly
(`esp_ota_set_boot_partition` → ota_0 at the new 0x180000), and a power cycle
returns to the launcher with all slot states intact — the rollback model is unaffected.

Not covered: booting `ota_1`/`ota_2` (identical code path to ota_0, only the partition
handle differs), the delete flow, and ota_2's audio-storage half of the dual-use
convention (no recording child firmware exists yet).

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
| 2026-09-12 | 3-Slot architecture (feat/shrink) | keep 2-slot x 2MB | factory shrunk to 1.44MB; cardid-before gap reused as ota_0; ota_2 enlarged to 2.61MB for dual-use storage |
| 2026-09-12 | Dynamic blob offset per slot | fixed 0x1FF000 offset | slot sizes now differ (0x1D6000/0x200000/0x29E000); blob offset = partition_size − 4KB |
| 2026-09-12 | -Os compiler optimization + WARN log | keep -Og Debug | -Os reduces binary ~20%; INFO log strings consume ~50KB .rodata |
| 2026-09-12 | LVGL examples/demos trimmed | keep full LVGL | default build compiles 1800+ demo units (~2MB); launcher only needs label/button/panel |
| 2026-09-12 | ota_2 dual-use: firmware slot or audio storage | separate storage partition | runtime check esp_image_verify(); littlefs ignores partition type; no partition-table conflict |
| 2026-09-14 | Remove LONG2; one LONG threshold (1.5 s) | keep two-tier LONG/LONG2 | LONG meant "back" on most pages but "confirm boot" on the warning page — opposite semantics at the same hold duration confused users |
| 2026-09-14 | Unsigned boot confirm = BOOT / CANCEL menu (UP/DOWN select, OK click confirm, default CANCEL) | OK LONG to confirm | same "UP/DOWN select + OK click confirm" model as the list and detail pages; keeps LONG = back everywhere |
