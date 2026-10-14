# USB Segmented-Session Backup Read — Real-Device Test Report (2026-10-14)

Language: [简体中文](usb-backup-segmented-session-test-report-2026-10-14.zh_CN.md)

## Verdict

**PASS.** The production USB page backup button (`#btn-backup`) now completes a full APP+DATA+NVS archive in a single page session on the host where the single-session reader previously died mid-read. One click, no reloads, no host-side assembly.

## What changed under test

Commit `db39bf8` on `feat/storage`, fixing `ee64d79 fix(backup): renew serial session between bounded flash read segments`: the page splits backup reads into 512 KiB segments and rebuilds Transport/ESPLoader between segments; on any session-restart failure it aborts without offering an incomplete ZIP.

## Results

| Criterion (handoff) | Result |
|---|---|
| One click on production UI; no manual reload / host segmented script | PASS — driver clicked `#btn-backup` once; single persistent page session |
| Archive complete, exactly one APP group + owned DATA | PASS — `manifest.complete=true`, manifest_version 2, one slot + one DATA record + NVS |
| Payload byte counts and SHA-256 accurate | PASS — 3/3 in-page verified; ZIP SHA-256 `3b578dd15f85…` (page-computed value identical) |
| Independent esptool.py dump byte-equal to archive | PASS — APP firmware `f519f076…`, slot tail `f78752cb…`, DATA `cba83ea5…` all byte-equal (tail = last 4 KiB of slot holding the `MNAM` name record; intra-slot 4 KiB 0xFF padding intentionally not archived) |
| Ownership metadata unchanged before/after backup and across reboot | PASS — LAN listing identical at all three checkpoints (normal DTR/RTS reset, not power-cut) |
| Any segment/session failure must fail the backup, never a partial ZIP | PASS by design demonstration — the known host transport stall fired once at `0x3d8000` and the in-page session renewal recovered; no partial ZIP path observed |

Backup: 577.9s @115200, archive 6,325,913 bytes. Per-read pace flat (~2.85 s/32 KiB).

## Session renewal fired in production conditions

At `0x3d8000` (DATA, ~22 min in) the reader hit `Packet content transfer stopped` — the exact failure that killed all four single-session attempts on this host (see [Stage C of the previous report](usb-data-backup-read-test-report-2026-10-14.md)). The page logged `DATA backup: opening a fresh serial/loader session at segment boundary`, re-synced a fresh loader, verified carve metadata was unchanged, and continued to a complete archive.

## Incidents

1. **Restore (scenario setup) failed twice at 921600**: the first 4 KiB carve-record reads timed out after the baud switch on both sectors and both connections, the page fell back to legacy mode and (correctly) refused transactional restore. Forcing 115200 via the harness-supported `state.baud` succeeded in 55.4s. This is a connect-time first-read flake, distinct from the mid-backup stall. The backup itself ran at 115200 throughout.
2. `usb_data_backup_read_once.mjs` does not start the dev server; it must be started with `META_PASS_DEV=1` (otherwise `mock-device.js` 404 blocks `__installSlotReady`). Two failed driver attempts precede the successful run; evidence preserved.

## Gate

Read-only gate respected: no delete, no restore-after-backup. The Play 28 group stays on the device.

## Evidence (local, gitignored)

`tools/realdevice/logs/20261014T032316Z-usb-backup-segmented-session/` — `report.md` / `report.zh_CN.md` / `report.json`, `environment.txt`, `install-rundir/` (group re-install), `backup-rundir/` (driver result JSON with per-read stats, ZIP, manifest, page log, console trace incl. session renewal, `t6-byte-compare.json`, esptool dump logs, `t7-post-reboot-slots.json`).

## Environment

Device firmware unchanged: `meta-pass_v2.1.0-1109-g…` factory (sha256 `377aefcf…`). Host: macOS darwin 24.6.0, Chrome 155.0.8059.40, Node v26.3.0, esptool 5.4.0, ESP-IDF 5.5.3. Token / IP / serial redacted.
