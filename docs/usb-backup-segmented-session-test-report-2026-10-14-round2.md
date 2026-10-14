# USB Segmented-Session Backup — Round 2 Real-Device Report (2026-10-14)

Language: [简体中文](usb-backup-segmented-session-test-report-2026-10-14-round2.zh_CN.md)

## Verdict

**PASS.** Success path completes in one click with flat per-chunk timing; failure path retries a failed chunk at the same offset after a session rebuild, caps at exactly 3 attempts, and fails explicitly with no ZIP. One environmental caveat documented below.

## What changed under test

HEAD `18ed1ae` (v2.1.0-1131). Since round 1 (`db39bf8`), the layered read recovery was removed: recovery is now a single mechanism — on a failed/timed-out 32 KiB chunk, restart the serial/loader session and retry the exact offset, max 3 attempts (15 s hard timeout per chunk); exhaustion aborts the backup (`b2c76f7`, `624273d`).

## Results

### Success path (attempt 3)

- One click on `#btn-backup`, single persistent page session. **PASS in 579.8 s**; archive 6,325,913 B, SHA-256 `f520cb52f4f536d0…`.
- Per-chunk timing: 194 successful 32 KiB reads — min 1786 ms, max 3050 ms, avg 2844 ms. By area: APP firmware ≈ 128 s (45 chunks), NVS ≈ 2 s, DATA ≈ 421 s (148 chunks), 11 session renewals ≈ 29 s; sum ≈ measured 580 s.
- Sessions: 12 ESPLoader syncs = 1 initial + 11 scheduled 512 KiB renewals (2 for APP firmware + 9 for DATA — exact segment math). Carve-metadata guard verified on every renewal.
- `manifest.complete=true`; payloads 3/3 in-page verified; ZIP hash equals page-computed value.

### Failure path (attempts 1–2, real stalls on this host)

| Attempt | Wall | Read errors | Failure-driven rebuilds | Terminal chunk | Final error | ZIP |
|---|---|---|---|---|---|---|
| 1 | 227 s | 6 | 5 | `0x3a8000` attempt 3/3 | `Backup failed: No serial data received.` | none |
| 2 | 190 s | 4 | 3 | `0x398000` attempt 3/3 | `Backup failed: No serial data received.` | none |

- Every failed chunk was retried at the **same flash offset** after a session rebuild — resume-from-failed-offset confirmed in production.
- Per-chunk retry cap is exactly **3 attempts** (`1/3`, `2/3`, `3/3` markers all observed), matching `BACKUP_RETRIES=3`.
- Terminal failure is explicit (error class, clear message) and **no incomplete ZIP is offered**.

### Independent verification

- esptool.py read-only dumps byte-equal to archive payloads: APP firmware `f519f076…`, slot tail `f78752cb…`, DATA `cba83ea5…`; intra-slot padding all-0xFF as designed (`t6-byte-compare.json`, PASS).
- Normal DTR/RTS reboot: LAN listing byte-identical to pre-backup; group metadata unchanged.

## Environmental caveat

After the first stall, attempts 1–2 stalled on nearly every chunk even in fresh sessions (Chrome WebSerial host transport death). An esptool probe immediately after attempt 2 read 512 KiB at 808 kbit/s with zero errors — device and link healthy. Attempt 3, same code and device, ran 580 s with zero stalls. The simple recovery survives isolated stalls (round 1 demonstrated) but cannot compensate for a persistently degraded host browser transport; it fails cleanly, which is the specified behavior.

## Gate

Read-only: no delete, no restore. Play 28 group left on the device.

## Evidence (local, gitignored)

`tools/realdevice/logs/20261014T052317Z-usb-backup-segmented-round2/` — `report.md` / `report.zh_CN.md` / `report.json`, `environment.txt`, `pre-backup-slots.json`, `failure-retry-analysis.json`, `t7-post-reboot-slots.json`, and per-attempt rundirs (`backup-rundir`, `backup-rundir-attempt1-fail`, `backup-rundir-attempt2-fail`) each with driver log, `backup-read-result.json`, console log, ZIP/manifest (PASS only), failure state + screenshot (fail runs), dumps and `t6-byte-compare.json`.

## Environment

Device firmware unchanged: `meta-pass_v2.1.0-1109-g…` factory (sha256 `377aefcf…`). macOS 15.7.4, Chrome 155.0.8059.40, Node v26.3.0, esptool 5.4.0, ESP-IDF 5.5.3. Token / IP / serial redacted.
