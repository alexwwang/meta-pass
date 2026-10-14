# Real-Device Test Report | USB DATA Backup Read (Stage A–C, gate closed)

[Chinese](usb-data-backup-read-test-report-2026-10-14.zh_CN.md)

- Date: 2026-10-14
- Handoff: `docs/HANDOFF-usb-data-backup-read.md`
- Branch/commit tested: `feat/storage` @ `ec7d956` (actual pulled HEAD, recorded)
- Verdict: **FAIL** — Stage C backup integrity not achieved; Stage D deliberately NOT executed.

## Stage A — host and simulator (PASS)

- `./tools/validate.sh` full: PASS, incl. firmware build `meta-pass_v2.1.0-1109-gec7d956.bin`.
- `./tools/validate.sh --sim` (passport-sim): PASS (5.1s).

## Stage B — frozen environment

- Firmware SHA-256: `377aefcf…8dcdb` (factory 0x10000+0x170000 via esptool read-flash; unchanged from prior sessions)
- Host: macOS (darwin 24.6.0), Chrome 155.0.8059.40, Node v26.3.0, esptool 5.4.0, ESP-IDF v5.5.3
- Serial `/dev/cu.usbmodemXXXX` (redacted), device 192.168.0.14, page `dev-v2.1.0-1109-gec7d956 · chunk=32KB stop-and-wait baud=921600(fallback 115200)`
- Baseline was an empty device; the Play 28 group (APP `REC-P28-HUMAN-GATE` slot 0 @1572864+1462272, play_id 2147483648; DATA `recordings` @3538944, 4841472B) was re-created via a `?noverify` restore of the previously validated archive (47.0s, write path unchanged). Known-good DATA payload hash `cba83ea5…`.
- Environment findings: the pre-pull dev-server process served a stale static whitelist (new `flash-readback-verify.js` module 404'd, page never became ready) — restarted from current checkout; a prior `esptool read-flash` had left the chip in bootloader mode (RTS pulse revived, no data impact).

## Stage C — read-only DATA backup: two natural reproductions, both FAIL

Single fresh page session per attempt, page `readFlashChunked` (32KiB stop-and-wait) at 115200, Debug mode on, no reconnect/reload/segmentation workarounds.

| | Attempt 1 | Attempt 2 |
|---|---|---|
| Successful reads / bytes | 89 / 2,895,872 | 44 / 1,441,792 |
| Died at offset | 0x4c8000 (≈1.47MB into DATA) | 0x490000 (right after APP-slot read) |
| First error | `Packet content transfer stopped` | `Packet content transfer stopped` |
| Page resync (drain 803ms → sync) | every retry `No serial data received` | same |

- Per-read durations flat (~2,853ms median per 32KiB) up to the death — abrupt transport death, no degradation. esptool-js TRACE tail: `Read 254 bytes: (empty)`, `Read 77 bytes: (empty)`, then `Packet content transfer stopped` (stub stopped emitting mid-packet).
- **The "~30 readFlash calls" characterization does not hold for read-only backup traffic**: death occurred at 45 vs 90 successful calls across two sessions; the historical ~30-call observation (restore path, preceded by write traffic) falls inside this scatter.
- **Control experiment (attribution)**: after the browser released the port, esptool.py read the full DATA range `0x360000 + 4841472B` at the SAME 115200 baud in **21.7s with zero errors**, SHA-256 `cba83ea5…` = known-good payload. Device, cable, baud and flash content are healthy → **failure is browser-side (Chrome WebSerial / esptool-js transport)**.

## Gate decision

No complete archive was produced in either attempt, so payload-hash verification could not run; per the plan, Stage D (delete, restore, byte-for-byte compare, reboot persistence) was **not executed**. The APP+DATA group remains on the device untouched (verified via LAN after each attempt; each dead backup leaves the chip in stub mode, revived via RTS pulse).

## Not done, per instructions

No chunk-size change, no second reader, no page-reload segmentation, no `flashMd5sum` substitution, no manual partition erasure, no delete/restore stages.

## Tooling committed

- `tools/realdevice/usb_data_backup_read_once.mjs` — the Stage-C natural-reproduction driver (UI-driven single attempt, live per-read log capture, failure classification `USB_READ_TIMEOUT|USB_READ_PROTOCOL`, offline archive validation on success).

## Evidence (local, gitignored)

`tools/realdevice/logs/20261014T005837Z-usb-data-backup-read/` — `report.md`/`report.zh_CN.md`/`report.json`, `environment.txt`, `attempt1-*`/`attempt2-*` (per-read page logs, esptool-js TRACE console, final-state JSON, screenshots, parsed read stats), `esptool-read-data.log` + `esptool-data-0x360000.bin` (control), `restore-rundir/` (group re-creation, restore PASS 47.0s).

## Follow-ups (not executed)

- Root-cause the Chrome/esptool-js transport death (ACK/response waits, call state) on this host class, or run backup via the already-proven segmented fresh-session approach (`p28_chunked_backup.mjs`) if the chain must be validated end-to-end.
