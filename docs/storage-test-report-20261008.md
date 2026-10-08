<p align="right">
  <a href="storage-test-report-20261008.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Storage Branch Test Report — feat/storage

**Date**: 2026-10-08
**Branch**: `feat/storage`
**Base commit**: `90c66f3` — docs(storage): clarify deferred reboot terminology
**Tested firmware commit**: working tree on top of `90c66f3` with two
real-device-driven fixes (see §4): firmware SHA-256
`5518535173c7d3352400a134f1f05f5b17d3b92b840af5a3a21edcb07b14f36e`
(1,111,408 bytes, app image at 0x10000)

---

## 1. Simulator Test (passport-sim)

**Status**: PASSED (carried over from the first run of this report; unchanged)

```
✔ meta-pass full image boots in QEMU and renders the 240x320 screen
✔ QEMU CPU keeps running after boot (no exception stall)
✔ DOWN key moves the list selection (frame redraws)
✔ 4 consecutive DOWNs wrap the list without a crash
✔ UP/DOWN pairs move focus back and forth with a stable frame
```

Environment: Node.js v26.3.0, passport-sim local checkout,
`tools/sim/esp_emu_bg.wasm` (3.3 MB). Exit code 0.

---

## 2. Real-Device Smoke Test

**Final status**: **PASS — S0/S2/S3/S4/S5/S6 all green**
**Run**: `tools/realdevice/logs/20261008-121330/` (evidence package complete:
command.txt, stdout.log, stderr.log, uart.log, slots/status responses,
report.json, report.md, flash/{table.bin,store.bin,recordings.bin})
**Physical power loss**: NOT performed — reported as POWER_LOSS_UNTESTED per
handoff rule (soft reset ≠ power loss). S5 exercised esptool soft reset only.

### 2.1 Setup

| Item | Value |
|------|-------|
| Device | ESP32-C3 AI Passport, 8 MB flash |
| USB port | `/dev/cu.usbmodem142401` |
| MAC | `4c:11:ae:2f:67:ec` |
| LAN IP | `192.168.0.24` (AP "Hundhaus", WPA2, rssi −66/−67 dBm) |
| Flash procedure | app-only `write_flash 0x10000` — NVS, partition table, otadata and cardid untouched (no `erase-flash`) |
| IDF | v5.5.3-dirty; host Python env pinned via `IDF_PYTHON_ENV_PATH=idf5.5_py3.10_env` (the auto-detected py3.14 venv has a broken `pydantic_core` ABI — see §3 issue 1) |
| Node | `/usr/local/bin/node` (real Node; `~/.local/bin/node` is a bun wrapper that breaks `node -e argv`) |

### 2.2 Stage results

| Stage | Description | Result | Key evidence |
|-------|-------------|--------|--------------|
| S0 | Pool+store erase, boot, token restore from NVS | PASS | `carve loaded: fresh device (safe table)`, `session token restored across reboot`, install service ready, HTTP 401→200 with restored token |
| S1 | Pairing | PASS (token-reuse path) | `reusing persisted NVS token (no pairing/button needed)` — no physical button press needed |
| S2 | First APP+DATA install (carve proposal, resume probe) | PASS | `prepare#1 200 (carve idx=0 off=0x180000)`, `DATA[0] resume checkpoint offset=2048`, `DATA[0] done offset=4096`, `finalize 200`, reboot → `carve loaded seq=2 slots=1` |
| S3 | Second slot install | PASS | `prepare#2 200`, `finalize 200`, reboot → `carve loaded seq=4 slots=2` |
| S4 | Delete + ARCHIVED, then eraseData delete | PASS | remove slot0 → `carve loaded seq=6 slots=1`, DATA `recordings` state=2 (ARCHIVED); remove slot1 eraseData → `seq=7 slots=0` |
| S5 | Interrupt at 370,469 B, soft reset, idempotent resume | PASS | service auto-recovered, old token still valid, idempotent re-prepare 200, `finalize 200`, reboot → `carve loaded seq=10 slots=1` |
| S6 | Flash readback + DATA byte compare | PASS | recordings.bin (4096 B) == initial image, SHA-256 `4e441a35…7205ec`; table.bin + store.bin archived |

`report.json` status: PASS, soft_reset_tested: true,
physical_power_loss_tested: false, failure_category: None.

### 2.3 UART evidence (final run)

```
meta_carve: fresh device (safe table); no carve yet
install_local: session token restored across reboot
meta-pass: ready: APP slots=0 free=6766592B s0=0 s1=0 s2=0   (launcher banner)
meta_carve: carve committed: seq=1 slots=1 materialize=1   (S2 prepare)
meta_carve: carve loaded: seq=2 slots=1                    (S2 reboot)
meta_carve: carve committed: seq=4 slots=2 materialize=1   (S3 prepare)
meta_carve: carve loaded: seq=4 slots=2                    (S3 reboot)
meta_carve: carve loaded: seq=6 slots=1                    (S4 archive remove)
meta_carve: carve loaded: seq=7 slots=0                    (S4 eraseData remove)
meta_carve: carve loaded: seq=10 slots=1                   (S5 resume+finalize reboot)
```

---

## 3. Issues found during testing (all resolved in-session)

### Issue 1 — ENV: IDF Python env auto-detection broken (pre-existing)

`source ~/esp/esp-idf-v5.5.3/export.sh` auto-detects Python 3.14.7 and activates
`idf5.5_py3.14_env`, whose `pydantic_core` ships a `cpython-310` native module
(ABI mismatch) → every `idf.py` invocation dies with
`No module named 'pydantic_core._pydantic_core'`. The smoke harness spawns
`idf.py monitor` through exactly this path, so UART capture was dead on arrival
in the first run of the day.

**Workaround (no repo change)**: export
`IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.10_env` (verified
healthy: pydantic_core 2.46.5 imports) before running the smoke.

**Recommendation**: reinstall the py3.14 venv
(`idf_tools.py install-python-env`) or delete it so auto-detection falls back
to a healthy env.

### Issue 2 — FIRMWARE (fixed): `/api/install/status` JSON truncated

**Symptom** (run `20261008-115132`): S2 crashed in the test harness with
`json.decoder.JSONDecodeError: Expecting ',' delimiter: line 1 column 283`
parsing the status response after a DATA prefix upload.

**Root cause** (`main/meta_store_install.c`, `h_install_status`): the response
tail `]}` was appended via `snprintf(body + off, …)` but `off` was never
advanced past it; `httpd_resp_send(req, body, off)` then sent a body missing
its final two bytes. The sibling `h_install_session` used
`httpd_resp_sendstr` (strlen-based) and was correct.

**Evidence**: probed device returned 282-byte body ending
`…"done":false}` — exactly 2 bytes short.

**Fix**: status handler now uses `httpd_resp_sendstr` like the session
handler. Sibling call sites grepped: only these two build JSON this way; no
other copy needed the change.

### Issue 3 — FIRMWARE (fixed): `sync_states` wiped slot `play_id` → archive chain dead

**Symptom** (run `20261008-115953`): S4 failed —
`archive_slot_and_data failed: ESP_ERR_INVALID_STATE` in UART; DATA `recordings`
stayed state=0 (not ARCHIVED=2) after removing its slot.

**Root cause** (`main/meta_carve_flash.c`, `meta_carve_flash_sync_states`):
the runtime-table backfill rebuilds each slot entry with
`memset(&built, 0, …)` and repopulated only kind/offset/size/state/SHA/name.
`play_id` is record-side metadata the runtime table does not carry, so every
backfill commit (visible as `materialize=0` seq bumps after each install)
silently zeroed it. `meta_carve_flash_archive_slot_and_data` then hit its
`play_id == 0 → ESP_ERR_INVALID_STATE` guard.

**Evidence**: store sectors decoded from real flash (`read_flash 0x35A000`)
showed slot entries with `play_id=0` while the data record had `play_id=1`.

**Fix**: copy `play_id` into `built` during backfill. Regression test added
to `tests/test_meta_carve_flash.c::test_sync_states` (set play_id=42 → sync →
assert preserved).

### Issue 4 — HARNESS (fixed): S6 read length hex formatting

**Symptom** (run `20261008-120733`): S6 byte compare failed with a 1000-byte
readback.

**Root cause** (`tools/realdevice/smoke.py` S6): f-string emitted
`read_flash 0x2a0000 1000 …` — `{len(data_bytes):x}` rendered 4096 as `1000`
without a `0x` prefix, and esptool parsed it as decimal 1000.

**Evidence**: first 1000 readback bytes matched the fixture with 0 diffs —
flash content was correct, only the read was short.

**Fix**: emit `0x{len(data_bytes):x}`. No other `read_flash` length in the
file lacked the prefix (checked both call sites above it).

---

## 4. Code changes made during this test session

| File | Change | Reason |
|------|--------|--------|
| `main/meta_store_install.c` | `h_install_status`: `httpd_resp_send(req, body, off)` → `httpd_resp_sendstr(req, body)` | Issue 2 — off didn't count the `]}` tail |
| `main/meta_carve_flash.c` | `sync_states`: preserve `slot[i].play_id` in rebuilt entry | Issue 3 — record-only metadata lost on every backfill |
| `tests/test_meta_carve_flash.c` | regression assertion in `test_sync_states` | Issue 3 — play_id=42 survives sync |
| `tools/realdevice/smoke.py` | S6 read length `0x{len:x}` | Issue 4 — truncated flash read |

Host verification after fixes: full `test_meta_carve_flash` suite PASS
(all 12 scenarios including the new regression); sibling suites re-run via
`tools/validate.sh --static` — see §5.

## 5. Verification

- Real-device smoke: `tools/realdevice/logs/20261008-121330/` — exit 0,
  all stages PASS (output quoted in §2.2).
- Host tests: `test_meta_carve_flash` full file PASS (not just the new
  assertion); whole `--static` suite (doc rules, key consistency, all C host
  tests, actionlint) green after the report docs were added.
- Firmware rebuild + app-only flash verified by boot log
  (`carve loaded`, `LAN install service ready`) and the passing run itself.
- Consistency greps: JSON-response builders (issue 2) and `read_flash` length
  args (issue 4) checked for sibling copies — none remaining.

## 6. Conclusion

- **Simulator**: PASS (unchanged from first run).
- **Real device**: **PASS** — S0–S6 all green on the final run; DATA byte-level
  flash verification PASS; reboot recovery PASS (soft reset).
- **POWER_LOSS_UNTESTED**: no physical power cut was performed; S5 used esptool
  soft reset only. Do not report this as power-loss durability.
- **Untested items**: physical button navigation (manual check noted by the
  harness), physical power-loss durability, USB-web install page
  (install-slot) end-to-end against this firmware.

---

*Report by omp coding agent — 2026-10-08. Prior blocked run by Agnes
(Sapiens AI) preserved in git history of this file.*
