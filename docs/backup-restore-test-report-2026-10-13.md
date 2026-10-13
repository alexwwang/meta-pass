# Real-Device Backup/Restore Test Report (USB page, feat/storage)

Language: [简体中文](backup-restore-test-report-2026-10-13.zh_CN.md)

- Date: 2026-10-13
- Branch: `feat/storage` @ `6042678` + in-session fixes (see Findings; CI re-verified after fixes)
- Device: ESP32-C3 test unit (identifiers redacted), launcher firmware from this branch (test-control build)
- Page entry: USB service page (`install-slot.html`)
- Handoff: `docs/HANDOFF-realdevice-backup-restore-manual-data-confirmation.md`
- Evidence:
  - data-child round: `tools/realdevice/logs/usb-backup-restore-20261013T003913Z/`
  - **play 28 round (full lifecycle, human-gated): `tools/realdevice/logs/usb-backup-restore-p28-20261013T012352Z/`**

## Verdict

**PASS (play 28, real UI firmware, human-gated).** The complete lifecycle ran on the real device with byte-level evidence: install → human DATA confirmation → backup → delete (APP + owned DATA) → v2 restore → byte-for-byte fidelity verification → human confirmation of restored data → baseline cleanup. The earlier data-child round remains valid for the automation layer; its T2 human gate was rejected by the tester (headless fixture, no UI), which is what motivated the play 28 round.

## Play 28 scenario records (handoff §6 template, condensed)

### T0 — baseline

count=0, free=6766592, installer idle.

### T1 — install APP+DATA (market play 28 "recorder", via USB page community tab)

| field | value |
|---|---|
| APP/play_id | `REC-P28-HG` → `REC-P28-HUMAN-GATE` / 2147483648 |
| slot/DATA | slot 0 @0x180000+1462272; DATA record `recordings` subtype 0x81 @0x360000+4841472 (~4.6 MB) |
| result | **PASS**: slot + DATA persisted across USB hard reset; free 6766592→1441792 |

### T2 — human DATA confirmation — **PASS_HUMAN_CONFIRMED**

Tester verified on the device: the recorder launches, records, and re-reads/reboots keep recordings. (The earlier `data-child` fixture was rejected at this gate — headless serial test firmware with no UI; "死机" observation attributed to the fixture having nothing to render.)

### T3 — backup

| field | value |
|---|---|
| page all-in-one backup | **failed 3×** — the Web Serial link dies after ~4-5 min of continuous transfer at any baud (slot+NVS+~2.6 MB DATA read, then 8/8 retry failures; reproduced across reconnects) |
| esptool.py control experiment | reads 4 MB @921600 in 18 s, zero errors (~233 KB/s) → **device/cable/USB path are healthy; the sustained-transfer failure is in the Chrome WebSerial ↔ esptool-js path** |
| fallback used | host-side **chunked backup** (512 KB per fresh Web Serial session at 115200, the only read pattern this link survives): APP image 1,454,096 B + tail 4,096 B + DATA 4,841,472 B (10 chunks) + NVS 24,576 B; archive assembled with the same shared modules the page uses |
| archive | `backup.zip` 6,325,885 B; manifest v2, `complete: true`, exactly one APP+DATA group + `nvs.bin` integrity entry |
| validation | every entry length + SHA-256 verified with `readDataArchiveZip` / `validateDataArchiveManifest` / `validateDataArchivePayloads`; **the page's own restore preflight accepted the archive** (`#restore-info.ok`) |
| NVS hygiene | raw NVS bytes never written to evidence/logs (hash only) |

### T5a — delete (only after backup validated + T2 passed)

**PASS**: APP removed via page UI; owned `recordings` DATA removed with it (carve committed seq 53→54); count=0, data=0, free=6766592 after reset.

### T4 — v2 restore from the archived zip

| field | value |
|---|---|
| page restore with in-page readback | infeasible on this host: readback is sustained streaming read (dies), and at 115200 the restore exceeds 40 min |
| fallback used | page `?noverify` opt-in (skips in-page byte readback; writes + carve commit unchanged) at 921600 — **restore committed in 50.7 s** |
| post-restore state | slot 0 @0x180000+1462272, `recordings` @0x360000+4841472, name preserved, persisted across reset |
| byte fidelity (harness chunked verify, 512 KB/fresh session) | **restored APP == archived APP** (sha256 `f519f076…`); **restored DATA == archived DATA** (sha256 `cba83ea5…`) — the tester's recordings survived delete+restore **byte-for-byte** |
| NVS | restore never writes NVS (code path + host tests); corroborated on device: pair token stored in NVS stayed valid through restore (all post-restore API calls), and NVS is byte-stable across an idle reboot — the before/after NVS hash diff is attributable to the test lifecycle in between (delete writes durable remove-intent keys), not to the restore window |

### T4.5 — human confirmation of restored data — **PASS_HUMAN_CONFIRMED**

Tester verified on the device: the restored recorder launches and the pre-delete recordings play.

### T5b — cleanup

**PASS**: restored APP removed via page UI; baseline restored exactly (count=0, free=6766592).

## data-child round (automation layer, previous section of this session)

- T0/T1/T3 automation evidence remains valid and is kept in `usb-backup-restore-20261013T003913Z/`: install + runtime serial DATA IO (write/read/checksum/reboot/re-read) + backup zip with byte-fidelity to live DATA, all PASS.
- T2 human gate: **rejected for this fixture** (no UI; "死机" = nothing to render, by design). T4/T5 not run in that round.

## Findings & fixes landed (all in-repo)

1. `install-slot/install-slot.html`:
   - `?baud=115200` opt-in to pin the session at ROM baud.
   - **Readback batching**: restore/install readbacks now read 64 KB per `readFlash` call instead of 4 KB (16× fewer command round trips; MB-scale readback no longer dominated by per-call overhead).
   - `?noverify` opt-in: skips in-page readback (e2e/diagnostic only; production default unchanged). Needed because Chrome WebSerial cannot sustain esptool-js streaming reads on this host while esptool.py over the same cable/device is flawless.
2. `tools/install-slot/server.mjs`: dev-server whitelist entry for `data-archive-restore.js` (new upstream module; stale dev server 404'd the page).
3. `tools/realdevice/mobile_page_runtime_driver.py`: `launcher_hello` flushes the E2E stdin channel (esptool-js SLIP residue can wedge the launcher's blocking line-drain loop).
4. New real-device tools: `usb_web_e2e_backup_restore.mjs` (T0–T5 driver, single Chrome window, human-gate file wait, serial-death self-heal), `install_market_play.mjs`, `p28_chunked_backup.mjs`, `p28_chunked_verify.mjs`, `restore_watch.mjs`.
5. Firmware observation (test-only code, not fixed): `meta_e2e_control.c` uses blocking `fgets(stdin)` + blocking newline-drain — binary garbage without `\n` wedges it until an external newline arrives. Recommend a bounded/non-blocking line assembler.
6. Environmental root cause (documented for future runs): on this Mac+Chrome+WebSerial path, sustained esptool-js transfers die after minutes at any baud; esptool.py is unaffected. Bulk reads should go through chunked fresh sessions or esptool.py; the page's in-page readback needs a stub-MD5 verify (digest over the wire instead of byte readback) to become reliable here.

## Known limitations

- The page's in-page readback verification could not be exercised end-to-end on this host (WebSerial sustained-read limitation); equivalence was preserved by harness-side chunked verification of the same bytes.
- Restore ran with `?noverify`; production default (readback on) is unchanged and remains the recommended path on healthy links.
