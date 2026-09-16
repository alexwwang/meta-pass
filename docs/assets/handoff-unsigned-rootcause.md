# Handoff — "Unsigned" Root-Cause Investigation for Child Firmware Flashed over USB

> 简体中文: [handoff-unsigned-rootcause.zh_CN.md](handoff-unsigned-rootcause.zh_CN.md)
>
> 2026-09-16. For the next session: read `docs/assets/meta-pass-signing-design.md` (the standard design)
> first, then this file.
> Branch `feat/sign`. Parent firmware `08128a0` (latest, contains all fixes). Child firmware
> `pass-radar_v0.1-2-g8fcce59-signed.bin`.

## 0. Resolution (2026-09-16, confirmed by on-device test)

**The issue is resolved.** After re-flashing the signed child firmware through the fixed install page,
on-device signature verification behaves as expected (SIGNED, no warning page). The root cause was the
**stale install page deployed on https://meta-pass.pages.dev/** (pre-dbbd091), not the signing chain:

- The signing chain (key chain, image_len semantics, digest range, device verifier) was verified correct
  end-to-end — see §2.
- The deployed page was a pre-dbbd091 copy: three-write flow writing only the 81-byte MSIG blob (not the
  full 4096 B tail sector), legacy `signature`/`signatureOffset` API, no `& 1` on `hashAppended`, and a
  stale 2-sector (8 KB) size reserve. Installing through it left the tail sector in a state the device
  reads as unsigned (§3).
- The fix that closes the reproduction path is BUG-03 in `docs/BUGS.md` (single-write tail-sector flow,
  `& 1` bit test) plus serving the canonical `install-slot/` from `server.mjs` (single source, no more
  dev-copy drift). The device must be re-flashed from the fixed page; a slot previously damaged by the
  old page stays unsigned until re-written.

## 1. Reproduction Conditions (as observed before the fix)

| Variable | Value | Status |
|---|---|---|
| Parent firmware (device meta-pass) | `meta-pass/build/meta-pass_v0.2.2-37-g08128a0.bin` | **latest**, contains dbbd091 layout + 325d01f/aa25c52 fixes + 08128a0 |
| Child firmware (flashed) | `pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin` | **host verify PASS** (META_SIG_OK) |
| Install page | `https://meta-pass.pages.dev/` | **stale (pre-dbbd091)** — confirmed by fetching the live site |
| Result | boot showed "unsigned firmware" | expected: SIGNED |

## 2. Ruled Out (evidence closed)

- **Key chain**: Keychain public point `04299a8d…ab651d13` == `public.pem` == embedded DER in
  `meta_sign_pubkey.h`/`metapass_hook.h` (91 B byte-equal). The v0.1-2 signature verifies **Verified**
  against `public.pem` via openssl. Four-way key sync holds; key drift ruled out.
- **image_len semantics**: IDF v5.5.3 `esp_image_format.c` is authoritative — `process_segments`
  (24+Σ(8+data_len)) → `process_checksum` (ALIGN_UP(+1,16)) → `process_appended_hash_and_sig`
  (hash_appended=1 → **unconditional** `image_len += 32`, line 974, invoked on the `esp_image_verify`
  path at line 207). Device image_len = 962416, identical across script/C/JS. The +32 deviation is ruled out.
- **Digest range**: device `slot_sha256(part, image_len)` = sha256(flash[0:962416]); signing-script
  digest = sha256(file[0:962416]). openssl confirms the v0.1-2 signature is **Verified** for that digest.
  Digest-range error ruled out.
- **The child firmware file itself**: `run-verify-tests.sh` (compiles the real `meta_sign.c` + brew
  mbedtls) → **META_SIG_OK**; `meta_sign_detect_sector` true; MAEG "PASS-RADAR v0.1". The file is correct.
- **Parent firmware code version**: 08128a0 ≥ 74ef1a0 ≥ dbbd091 ≥ 994caaf; scan_one/meta_sign.c are the
  latest. Stale parent firmware ruled out.
- **Stale signed artifact mix-up**: the user flashed v0.1-2 (not v2). Ruled out.

## 3. Root Cause

### 3.1 The only active drift: the deployed install page was pre-dbbd091

`meta-pass.pages.dev/extract-app-image.js` (fetched live during the investigation):
- `hashAppended = buf[start + 23] === 1;` (no `& 1`, BUGS.md (c))
- returned `signature`/`signatureOffset` (old API), not `tailSector`/`tailSectorOffset`
- comments explicitly described a "third writeFlash" — **three-write flow** (app / 81 B signature /
  blob@part_size−4KB)
- `maxAppImageSize = partSize − SIG_SECTOR − BLOB_SECTOR` (old 2-sector design, 8 KB reserved)

The repo's `install-slot/` (commit dbbd091) already had the **single-write flow** (extract the full
4096 B tailSector, merge MNAM, one writeFlash for the whole sector), but:
- **pages.yml deploys from `main`**, main HEAD = 74ef1a0 (contains dbbd091), yet the live site lagged
  behind main (the fetched page was the old version).
- Working-tree fixes (the `tools/install-slot/` dedup and `server.mjs` serving `install-slot/`) were
  **uncommitted** at the time.

### 3.2 Key mechanism: the tail sector has no esp_image_verify safety net

IDF `verify_simple_hash` (lines 225-228, the non-Secure-Boot path) only validates `[0:image_len)` =
`[0:962416)` — the app portion. **The tail sector `[962560:966656)` is entirely outside
esp_image_verify's validation** — its content may differ from the file while IDF still judges the slot
valid (→ shows "unsigned" rather than "invalid").

The old page wrote only 81 bytes of signature into the tail sector (writeFlash #2); the remaining 4015
bytes relied on erase residue being 0xFF. **Host tests** verify against the full 4096 B tailSector inside
the file → PASS; the **device** reads the tail sector as written by the old page. The actual on-flash
content was the one thing no test covered — the only gap that could explain "host PASS + device FAIL".

### 3.3 Precise mechanism confirmation (flash dump, if ever needed again)

If a regression ever reappears, dump the device flash and compare with the file at offset 962560:

```bash
# Assuming ota_0 (0x180000); adjust to the actual slot
python -m esptool --port /dev/cu.usbmodem* read_flash 0x26B000 4096 /tmp/device_tailsector.bin
# 0x26B000 = 0x180000 + 0xEB000(962560)
xxd /tmp/device_tailsector.bin | head
```

- **all 0xFF** → the signature never landed (writeFlash #2 failed/skipped/wrong address) → META_SIG_ABSENT
- **81 B match, rest differ** → written correctly but damaged afterwards
- **identical** → tail sector is fine; capture the boot log line `slot N signature: unsigned (X)` for the sr code

This step was not needed in the end: re-flashing through the fixed page resolved the symptom, consistent
with the §3.1 root cause.

## 4. Action Items (status)

1. ✅ **Commit the fixed install flow**: single-write tail-sector page + `& 1` bit test (BUG-03 fix) and
   `server.mjs` serving the canonical `install-slot/` (dedup) — see `docs/BUGS.md`.
2. ✅ **Re-flash v0.1-2 via the fixed page** — on-device verification now behaves as expected (SIGNED).
3. If a recurrence ever shows "unsigned" again: run §3.3 flash dump and triage by sr code.
4. Clean up stale signed artifacts in `pass-radar/build/` (`*_v2.bin` etc.) to prevent mix-ups.
5. After redeploying pages.dev, verify its `extract-app-image.js` returns `tailSector` (not `signature`).

## 5. Minor Drifts (same chain, handle together)

- The `sign-firmware.sh` / `test_integration.c` parsers have **no 16 B extended-header probe**
  (`extract-app-image.js` has one) — not triggered by current images, but the contract is
  inconsistent. **Decision (2026-09-16, compatibility-first): port the same `[16, 0]`
  auto-detect into both parsers** so all three share the contract; rationale and established
  facts in `docs/development/engineering/debugging-workflow.md` §4. **Pending.**
- The Wi-Fi path (`meta_net.c`) has signature protection (`meta_sign_detect_sector` → never overwrite a
  signed sector's MNAM); the USB page relies on the JS extraction being correct instead.

## 6. Standard Design and Alignment Matrix

See `docs/assets/meta-pass-signing-design.md`. Alignment conclusion: signing script / host tests /
device-side C code / key chain **all conform to the standard**; the only active drift was the **deployed
install page** (pre-dbbd091) plus uncommitted/unfixed local copies — both now closed (§0, §4).
