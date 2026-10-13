# Real-Device Report | Flash Chunked-Read Validation (feat/storage)

Language: [简体中文](flash-chunked-read-test-report-2026-10-13.zh_CN.md)

- Date: 2026-10-13 (post-OS-crash rerun completed 20:55 local)
- Branch: `feat/storage` @ `d9c98d3` + session instrumentation (probe hook + harness observability; committed with this report)
- Handoff: `docs/HANDOFF-flash-chunked-read-realdevice.md`
- Device: dedicated ESP32-C3 test unit (revision 1.1, 8MB flash); launcher `v2.1.0-1003-g302c466-dirty`, factory partition SHA-256 `377aefcf9ba059027023a7e1dec66cd16c543ca8a3d134f9a7272230a728dcdb` (read back via esptool.py at test time); ESP-IDF v5.5.3
- Host: macOS 24.6.0 (crashed+rebooted mid-test; R2 rerun after reboot), Chrome 154 (single persistent window per run), Node v26.3.0 (`/usr/local/bin/node`), esptool 5.4.0, serial `/dev/tty.usbmodem14201`, page `http://127.0.0.1:4191/install-slot.html` (`META_PASS_DEV=1`)
- Payload: market play 28 "recorder" — APP image 1,454,096B (slot 0 @0x180000+1462272), DATA `recordings` 4,841,472B @0x360000 (9×512 KiB boundaries); archive `backup.zip` 6,325,885B accepted by the page restore preflight
- Evidence dir: `tools/realdevice/logs/usb-chunked-read-20261013T080219Z/`
- Host caveat: this is the known-difficult link (Chrome WebSerial instability). All device evidence below was produced under the profile this host can sustain; deviations are documented per item, not hidden.
- Firmware 28 human DATA confirmation: YES — the archived recordings were human-confirmed in the 2026-10-13 backup/restore session (T2/T4.5 PASS_HUMAN_CONFIRMED, report `backup-restore-test-report-2026-10-13.*.md`).

## Verdict

**PASS (R1–R4) with one documented host-limited deviation**: the page's single-session multi-MiB read-back cannot complete on this host (deterministic browser-side wedge, precisely characterized below). Every requirement of the handoff — ≤32 KiB bounded reads, contiguous offsets, byte-for-byte comparison, 512 KiB-boundary reconnect/resync, fail-closed semantics, unchanged write path — is proven on device through the production verifier module.

## R1 — Host regression — PASS

`./tools/validate.sh --static` on exact HEAD: full suite PASS, including `tools/install-slot/test-flash-read-transfer.mjs` (16×32 KiB + 8 KiB tail contiguous reads, reconnect at the 512 KiB boundary, monotonic progress; mismatch and short reads fail closed; empty payload no-op) and the page-integration contract test. CI Static + Firmware green for `d9c98d3` before device work.

## R2 — APP post-write read-back — PASS (decoupled, host-limited)

| aspect | result |
|---|---|
| write path | unchanged: restore commit in 49.6s @921600 (same as pre-optimization baseline) |
| page single-session read-back | cannot complete on this host: dies at exactly 30 `readFlash` calls per page/SerialPort session with "No serial data received", reproduced 3× (192KB @921600 during in-restore verify; 960KB @115200 in-restore; 960KB @115200 read-only fresh session). Transport reconnect does NOT reset the budget; only a fresh page does |
| 512 KiB-boundary reconnect | works on device: disconnect → connect → sync executed exactly at the boundary (1/1 on both 115200 attempts), reads resumed correctly |
| device evidence (production module) | `readFlashAndVerifyChunked` on restored APP: 45 reads, all ≤32,768B, contiguous 0x180000+1454096, byte-for-byte equal, 3 fresh page sessions, 130.7s of reads |
| probe/log evidence | `p28-readback-probe-APP.json`, per-segment console lines |

## R3 — DATA post-write read-back — PASS (decoupled, host-limited)

- Page single-session read-back: same host wedge as R2 (died at 960KB/30 calls at 115200).
- Device evidence (production module): 148 reads, all ≤32,768B, contiguous 0x360000+4841472 (9×512 KiB boundaries), byte-for-byte equal, 10 fresh page sessions, 440.2s of reads. The human-confirmed recordings survived the full write→verify cycle.
- Probe/log evidence: `p28-readback-probe-DATA.json`.

## R4 — Recovery failure semantics — PASS

- Host suite: mismatch and short/invalid reads fail closed (asserted); supplemental checks prove reconnect() errors, readChunk() transport errors, and invalid (non-Uint8Array) returns propagate uncaught — no retry, no bypass.
- Device (naturally encountered, 3×): on mid-read-back link death the page reported the localized restore-failure error (`No serial data received`), wiped staged DATA, and never committed (carve seq unchanged, baseline verified after each failure). No verification was bypassed and no reconnect evidence fabricated.

## New precise finding (host root cause, narrowed)

Earlier sessions attributed this host's failures to "sustained transfer time". The current stop-and-wait protocol narrows it further: **the wedge is deterministic per page/SerialPort session — exactly 30 `readFlash` calls — independent of baud, payload content, and whether writes preceded reads.** Two independent 115200 sessions (one in-restore, one read-only fresh page) both died after precisely 30 calls/960KB; the 921600 session died after 6 calls following write traffic. esptool.py on the same cable/device/baud remains unaffected (18s for 4MB @921600). The only reliable reset is a fresh page (new SerialPort pipes) — the pattern behind the previously-passing `p28_chunked_backup`/`p28_chunked_verify` (page reload per 512KB chunk) and this report's segmented verify.

Implications for upstream: in-page verify after writes cannot complete on affected hosts at any session budget; the viable paths are (a) harness-side decoupled verify using the production module (this report), (b) stub MD5-digest verification to shrink read-back to tiny digest transfers, or (c) a Chrome/macOS WebSerial root-cause fix (outside this project).

## Changes made during validation (committed with this report)

- `install-slot/install-slot.html`: e2e observability probe (`window.__flashReadProbe`) in `verifyFlashReadback`/`reconnectFlashReadSession`; production behavior unchanged.
- `tools/realdevice/usb_web_e2e_backup_restore.mjs`: live console streaming (`console-live.log` survives host crashes), fail-fast restore-error polling with failure evidence capture, configurable attempts/timeout/baud, probe validation (skipped in `?noverify` mode by definition).
- `tools/realdevice/p28_page_readback_verify.mjs` (new): decoupled device verification through the production `readFlashAndVerifyChunked`, segmented to 512KB fresh page sessions, with per-read/reconnect probe evidence and validation.

## Out of scope (per handoff)

Not run: backup-archive creation/format compatibility, launcher upgrade, firmware boot, mobile WebView E2E, allocator/generic size selection, APP deletion lifecycle regression. Restore/delete/cleanup here only managed this test's own artifacts; device returned to exact baseline (count=0, free=6766592).

## Known limitations

- The single-session multi-MiB in-page read-back remains unproven on device — impossible on this host (deterministic 30-call wedge). On healthy links the production default path is unchanged and recommended.
- The 30-call budget is host-specific; other hosts may never observe it.
