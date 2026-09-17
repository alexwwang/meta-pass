<p align="right">
  <a href="debugging-workflow.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Root-Cause Debugging Workflow — Lessons from the Signature-Verification Investigation

> Status: engineering rule (binding for AI-assisted and human debugging on this repo).
> Derived from the 2026-09 "child firmware shows Unsigned after USB flashing" investigation
> (BUG-01…04, BUG-03 install-path damage, stale deployed installer). Case history:
> `docs/BUGS.md`, `docs/assets/handoff-unsigned-rootcause.md`; contract:
> `docs/assets/meta-pass-signing-design.md`.

## 1. Case history (three rounds, what actually happened)

**Round 1 — static bug hunt against the design doc.** A full-branch review against
`docs/assets/meta-pass-design.md` produced BUG-01…04 (uninitialized battery label, inverted
egg-magic check in the `HOST_TEST` stub, dev-installer page drift, `%d` for `size_t`), each
verified with file:line evidence, plus a list of *cleared* suspects (LVGL timer self-delete,
scroll semantics, size-limit arithmetic, button long-press wiring) with the evidence that
cleared them. Recording cleared suspects matters: the next person must not re-investigate them.

**Round 2 — "re-signed firmware still shows Unsigned".** The tempting hypothesis (signing
chain broken) was **disproven first**: the host integration test compiles the *real* firmware
verifier and reported `META_SIG_OK` on the newest signed image, and both launcher builds
embedded the current public key. The actual cause was the **install path**: the dev installer
page (the README's primary local workflow) wrote the display-name blob with a second
`writeFlash` into the same 4 KB tail sector that already held the signature — esptool erases
the whole sector on every write, so the signature was wiped at flash time even though the
`.bin` file was perfect. Structural fix: one canonical installer directory served by both
local dev server and Cloudflare deployment; the tail sector is always written in a single
`writeFlash`.

**Round 3 — closing the "host PASS + device FAIL" gap.** Static analysis could not prove
what the *deployed* (stale) installer had actually put on flash. Instead of guessing, the
installer's byte path was replayed on the host: extract → MNAM patch → assemble slot bytes →
feed to the real firmware verifier (real mbedtls) → `META_SIG_OK`. Later the user confirmed
on-device behavior as expected after re-flashing through the fixed page. The general lesson:
**when host and device disagree, replay the exact byte transformation on the host and feed the
result to the device-side code compiled for the host** — this converts an unfalsifiable
"maybe the flash content differs" into a concrete pass/fail.

**Round 4 — backup always fails with "Packet content transfer stopped / No serial data
received", retries never recover.** Three rounds of plausible-looking patches (chunked read,
resync-before-retry, pipelined ACK window) did not fix it, and two of them introduced
regressions (an oversized read block that the stub silently ignores → dead session; a probe
with no timeout → stuck serial port). The real root cause was found only by reading the
**stub source** (`flasher_stub/stub_commands.c`): `handle_flash_read` unconditionally appends
a 16-byte MD5 digest frame after the data frames, and the battle-tested reference
(`esptool.py read_flash`) reads and verifies it — while **esptool-js (all versions, including
our vendored copy) never reads that frame**. The leftover digest frame stays in the transport
buffer and poisons the next command's response → protocol desync that accumulates per
`readFlash` call → bulk reads (backup) always fail mid-stream, and every retry inherits the
desynced buffer, which is why blind retries never recover. Writes never hit the code path,
which is why install/upgrade always worked while backup always failed. Fixes: per-frame ACK
(esptool.py semantics), read + verify the digest frame (using a local MD5), block size ≤ 4 KB
(the stub's hard limit; larger values are silently ignored), and a protocol-level unit test
that replays a mock stub (`tools/install-slot/test-readflash-protocol.mjs`). Meta-lesson:
**when a third-party client library disagrees with a working reference implementation on the
same protocol, read the server-side (stub/firmware) source and the reference client before
patching failure handling around the broken client** — retry logic cannot compensate for a
protocol-conformance bug.

## 2. Lessons (what to do differently)

1. **Verify the signer before the transport.** When "valid file, device disagrees", first
   prove the file itself against the *real* verifier (`tools/signing/run-verify-tests.sh`
   compiles `main/meta_sign.c` + mbedtls), then work backwards through the write path.
2. **The device reads flash, not files.** Any test that validates a `.bin` says nothing about
   what an installer put at the slot offset. End-to-end means: source file → installer
   transformation → assembled slot bytes → device verifier.
3. **One sector, one write.** esptool/esp_ota erase per write; two writes to one 4 KB sector
   = silent loss of the first. This is why the tail sector (MSIG/MAEG/MNAM) must be assembled
   in memory and written once (`meta-pass-signing-design.md` §7.1 single-write contract).
4. **Copies drift; deployment lags.** The stale `https://meta-pass.pages.dev/` page caused the
   on-device symptom while the repo was already fixed. Two copies of any installer/page is a
   standing bug (BUG-03): serve the canonical directory (`server.mjs` → `install-slot/`), and
   after any fix, verify the *deployed* asset byte-for-byte, not the repo one.
5. **Duplicate the cleared-suspects list.** A cleared suspect with evidence prevents repeated
   dead ends (BUGS.md "cleared" section; handoff §2 "ruled out").
6. **Asymmetric strictness at the trust boundary.** Parsers that accept third-party binaries
   (the install page) may auto-detect layout variants; parsers inside the signing/trust chain
   (script, host test, device) stay strict to one layout. See §4 below for the 16 B
   extended-header decision.
7. **Host-test variants are product code.** `#ifdef HOST_TEST` branches are compiled by
   `validate.sh` on every run — a bug there (inverted magic, BUG-02) is a real defect even if
   no device path executes it. Give them regression tests like any other code.

## 3. Standard workflow for device-behavior bugs

1. **Reproduce cheaply, on the host if possible.** Prefer: unit test → host integration test
   with the real module (`run-verify-tests.sh`, `test_meta_net_upload.c`) → QEMU simulator
   (`tools/sim/`) → only then on-device.
2. **Close the evidence loop before fixing.** For each layer (key chain, image_len semantics,
   digest range, layout offsets), record the check that rules it in or out — see handoff §2
   for the format.
3. **If the bug survives static analysis, dump or replay bytes.** Flash dump triage script and
   the branch-on-result table live in `handoff-unsigned-rootcause.md` §3.3.
4. **Fix the class, not the instance.** The signature-wipe fix also removed the duplicate-page
   class (single source) and added regression tests locking the layout (m1–m4 in
   `tests/test_meta_net_upload.c` interlock with `sign-firmware.sh`).
5. **Deliver with the full gate and write the case down.** `tools/validate.sh --static`, then
   update `docs/BUGS.md` / handoff docs (bilingual) in the same change.

## 4. The 16 B extended-header question — context and decision

**Context.** The four-way contract (`meta-pass-signing-design.md` §8) requires the signing
script, host test, install-page JS, and IDF's `esp_image_verify` to derive byte-identical
`image_len`. The install page additionally auto-detects a hypothetical "16 B extended header"
after the 24 B `esp_image_header_t` (`extract-app-image.js` tries `[16, 0]` layouts and takes
the one whose segment table walks cleanly — the two layouts are mutually exclusive, so exactly
one can converge). The script and host test parse the plain 24 B layout only.

**Facts established (IDF v5.5.3 + real image).**
- `esp_app_format.h:110` asserts `sizeof(esp_image_header_t) == 24` (packed); official docs
  show segment 0 at file offset `0x18` = 24. The 24 B header *internally* contains what
  esptool calls the "Extended Image Header" (WP pin, flash-pin drive settings, chip_id,
  min/max rev — bytes 8..23). That naming trap is likely the origin of the "16 B extra"
  belief; it is part of the 24 B header, not appended after it.
- Real pass-radar image: segment 0 header at 24 reads `load_addr=0x3c0b0020` (valid C3 DROM),
  `data_len=147660`; image_len 962416 matches across all four parties. The ext=16 probe never
  wins for official-toolchain images — it sleeps.
- Even if such an image reached the device, IDF itself has no such probe: `esp_image_verify`
  would read a bogus segment table at offset 24 and judge the slot INVALID. The device cannot
  and should not accept non-standard layouts.

**Decision (compatibility-first, adopted 2026-09-16).** Port the same `[16, 0]` auto-detect
into `sign-firmware.sh` (`compute_esp_image_len()`) and `tools/signing/test_integration.c`
(`esp_image_len()`), so all three parsers share the contract and the §8 table stops listing a
known inconsistency. Rationale: the install page is the open input boundary for third-party
binaries; keeping the signing/verify chain able to *parse* the same variants (with the
device's `esp_image_verify` remaining the final arbiter that rejects malformed images) avoids
a contract split where the page accepts something the signer cannot process. The probe is
dormant for official-toolchain images, so the change is behavior-neutral for current builds.
Tracked as an action item in `handoff-unsigned-rootcause.md` §4; not yet implemented.

**Implementation note (2026-09-16, done).** All three parsers now probe `[16, 0]`. Locking
tests: `test_integration.c --selftest` (plain-24B → 240, 24B+16B-ext → 256; fixtures 0xab-filled
so a wrong layout reads a huge `data_len` and falls through) and `test-extract.mjs` PASS 3c
(same fixtures + an unresolvable-truncation negative case). A three-way replay over the
synthetic fixtures confirmed identical results across Python/C/JS. Fixture pitfalls worth
remembering: `data_len` must be written as a full u32 LE (a one-byte store leaves 0xab in the
upper bytes and silently turns the length huge), and C test buffers must be memset — the
equivalent JS fixtures pass because `Uint8Array.fill` initializes everything. Re-signing the
real pass-radar image after the change still produces image_len 962416 / `META_SIG_OK`.

### Round 5 (vendored esptool-js transport): hidden self-destruct via timers & promises

**Lesson: a library's `finally{}` blocks and pending promises can kill you long after you
thought they were done.**

Backup reads died at ~2 min and the retry recovery hung silently for 30+ minutes. Three
rounds of chunking/retry/ACK patching missed it; only a line-by-line read of the vendored
minified bundle exposed two structural defects:

1. `readLoop`'s `finally{buffer=new Uint8Array(0)}` wipes the transport buffer when its
   timeout fires — and an abandoned generator's timeout timer still fires later, swallowing
   the tail of an in-flight stream. With `FLASH_READ_TIMEOUT=100s` this matches the observed
   "dies at ~2 min" exactly; it had been misdiagnosed as USB jitter.
2. `flushInput()` starts with `await this.reader.closed`, which **never settles** on an
   active serial port — any recovery path reaching it hangs forever, without even an error.
   That was the "retry (1/5) then 30 minutes of silence".

Method takeaway: **when debugging a hang in a timeout chain, first audit the library's
internal `Promise.race` settlement paths and `finally` side effects before blaming the
environment**; when upper-layer retries/timeouts cannot revive it, there is almost always a
never-settling await inside the library. Fix patterns: persist the pending read
(`_pendingRead` reuse), close generators explicitly with `return()`, attach
`catch(()=>{})` to timeout timers against unhandled rejections, and put hard timeouts on
every recovery step. Also: a Debug mode in the page (per-chunk timing, recovery steps) beats
guessing from logs afterwards.
