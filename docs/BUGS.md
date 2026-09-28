# meta-pass — Bug Report (current branch)

English | [简体中文](BUGS.zh_CN.md)

Code review of the current branch against `docs/assets/meta-pass-design.md`.
Each entry: symptom → root cause → fix. Severity: **High** = data loss / feature
broken, **Medium** = wrong behavior or latent defect in real flows, **Low** =
robustness / hygiene. Suspects that were investigated and found correct are
listed at the end, with evidence.

| ID | Severity | Component | One-liner | Status (fix applied on this branch) |
|----|----------|-----------|-----------|------------------------------------|
| BUG-01 | High | `main/main.c` | Battery label built from an uninitialized stack buffer; SOC never rendered | **FIXED** — guard + `snprintf` at `main.c:69-71` |
| BUG-02 | High | `main/meta_sign.c` | Inverted egg-magic check in the `HOST_TEST` `meta_egg_parse` variant | **FIXED** (upstream commit) — regression tests `m1`–`m4` added to `tests/test_meta_net_upload.c` |
| BUG-03 | High | `tools/install-slot/` (dev copy) | Diverged from shipped installer: out-of-partition name-blob write, double-write erases signatures, header-flag drift | **FIXED + structural fix done** — dev page files deleted; `server.mjs` now serves the canonical `install-slot/` directly |
| BUG-04 | Low | `main/meta_net.c` | `%d` used for `size_t` in the upload-success log | **FIXED** — `%zu` at `meta_net.c:407` |

---

## Store OTA / provisioning failure chains (r9 cycle, 2026-09-27/28)

Found during real-device bring-up of the store download channel. Each entry:
symptom → root cause → fix → lesson. All five chains were invisible to the
simulator (no RF bridging) and to the existing host tests; the fixes that
apply are pinned by new host tests, and `tools/e2e-production.mjs` now guards
the data plane against the production site.

### BUG-05 (High) — scan residue state froze scanning *and* connecting

- **Symptom**: after one empty scan result, the phone never saw the target
  network again; submitting credentials then stalled in "Connecting to WiFi…"
  for the full 30s deadline and fell back to the AP.
- **Root cause**: `/api/scan` split "start scan" (network task) from "read
  results" (HTTP handler), and the handler returned **without calling
  `esp_wifi_scan_get_ap_records` whenever count == 0**. In ESP-IDF, a finished
  but un-drained scan leaves the driver in a residue state that blocks the
  next `esp_wifi_scan_start` **and** `esp_wifi_connect`.
- **Fix**: scan start → bounded wait → **unconditional** drain moved into one
  handler-synchronous step; cross-task handoff removed.
- **Lesson**: with IDF WiFi, *reading the results is part of finishing the
  scan* — every scan path must drain, including (especially) the empty one.

### BUG-06 (Critical) — a "fallback" retry that prevented association

- **Symptom**: (predicted by static review, confirmed on device) reconnect
  attempts never converged while the per-second retry loop was active.
- **Root cause**: self-inflicted. A per-second `esp_wifi_connect()` was added
  as a "lost-event fallback"; IDF treats re-entry while connecting as
  disconnect + reconnect, and each call restarts connect's internal channel
  scan — the retry loop *was* the reason association never completed. Event
  group bits are sticky, so the scenario the fallback guarded against does
  not exist.
- **Fix**: removed; disconnect events are trusted (sticky bits) and reasons
  are surfaced on screen via `meta_store_wifi_fail_text` (15/201/202/204/205).
- **Lesson**: never paper over event-delivery doubts with repeated side-
  effectful calls; check the API's re-entry semantics first (`esp_wifi_connect`
  is not idempotent) and the primitive's memory model (sticky bits) second.

### BUG-07 (High) — the 300s session overlay froze the provisioning panel

- **Symptom**: screen stuck on "Syncing clock…" for 200+ seconds while the
  `timeout in Ns` label kept counting; after reboot the device worked (the
  credentials had been saved all along).
- **Root cause**: the session-timeout overlay armed during provisioning (the
  job queue is idle then, `busy=false`) and, once shown, `store_tick` returned
  early every tick — the panel froze at its last frame. The running timeout
  label was the disproof of the first (wrong) "overlay freezes everything"
  theory; the correct chain is "tick alive → only `s_info` updates stale →
  overlay armed → early return".
- **Fix**: overlay suppressed during AP_UP/CONNECTING/ERROR; panel refreshes
  live with elapsed seconds ("Syncing clock… (7s)"); provisioning webpage no
  longer claims "saved!" ("Received…" + point at the device screen).
- **Lesson**: a stuck label with a *live* sibling label localizes the freeze
  to the update path of the stuck one — use that differential before
  theorizing. Time-factor check ("can any timeout in the code produce 200s?")
  would have falsified the first theory immediately.

### BUG-08 (High) — ONLINE transition never detected → no auto-advance

- **Symptom**: panel froze at "Syncing clock… (3s)" with the timeout label
  counting; the device never left the provisioning page although WiFi was up.
- **Root cause**: the 2s auto-advance timer was anchored at **page build time**
  (`s_net_online_at` captured once; 0 when the page was built during
  CONNECTING), so `s_net_online_at != 0` never became true; ONLINE also had no
  panel-refresh branch, so the last CONNECTING frame persisted.
- **Fix**: the tick detects the ONLINE *transition* itself (first sight →
  record timestamp + repaint panel), making the advance independent of when
  the page was built.
- **Lesson**: anchor timers to **state transitions**, not to page-build
  moments; every reachable state needs a render branch (a missing branch is a
  frozen frame, not a rare cosmetic gap).

### BUG-09 (High) — hard-reject policy locked out half the marketplace

- **Symptom**: five plays all showed unsupported on device within one day of
  r8 shipping.
- **Root cause**: market data changed under us — plays started shipping extra
  data partitions (`easter` subtype 0x82, voicefs-type 0x81) and the r8 rule
  hard-rejected any non-whitelisted data partition with subtype != 0x40. The
  policy premise ("installing would corrupt the device") was wrong: only the
  extracted factory app is written; partition payloads never enter the device.
- **Fix**: all non-whitelisted *data* partitions warn-and-allow with
  `detail=<label>` (pinned by analyzer PASS 5/5b/5c + firmware contract test);
  production baseline re-verified 563/675/200 the same day.
- **Lesson**: accept/reject policies must be re-validated against **live
  production data**, not last week's samples — the production baseline check
  (E2E-1/2 over golden IDs) is what caught this within hours.

### BUG-10 (High) — store page-flow contained an inescapable loop

- **Symptom**: from P1 there was no way out of the store: OK LONG went to P0,
  and ONLINE auto-advanced P0→P1 after 2s. The only real exit (P0 OK LONG →
  list) had a 2-second window.
- **Root cause**: the page flow was never drawn. Each transition was designed
  locally ("P1 long-press should go somewhere sensible" → P0), and the
  composition of P0's auto-advance with P1's exit created the cycle.
- **Fix**: canonical flow graph documented in plan §8-r10; OK LONG exits the
  store from every page; P0 auto-advance is cancelled by any keypress; P0
  shows an explicit `> CHANGE WIFI` row so changing networks does not require
  a connect failure first.
- **Lesson**: for any multi-page UI, **draw the transition graph before
  wiring keys** and check it for (a) an exit reachable from every node and
  (b) no cycle that traps the user. A composed flow is a property of the
  whole graph, not of individual edges.

### BUG-11 (High) — short-press navigation mapped to an invisible cursor

- **Symptom**: on the P1 keypad, short UP/DOWN presses appeared dead; only
  long-press row-wraps moved anything.
- **Root cause**: the r9 key-mapping change (short = move cursor) moved the
  *insert caret*, which does not render on an empty input — the visible key
  selection had no short-press path at all.
- **Fix**: short UP/DOWN = selection ring ±1 (`mpd_idedit_move_ring`,
  host-tested over all 15 keys both directions); on-screen ◀▶ keep cursor
  semantics. Also fixed in the same pass: P1 panel height 100→84 (keypad
  overlapped it by 10px) and OK key 58→64 so its bottom aligns with the 0 key
  — both locked with `_Static_assert` so geometry regressions fail the build.
- **Lesson**: key → action mappings must be reviewed against **what is
  visible on screen**, not against the model's internal state; and layout
  invariants (no overlap, edge alignment) belong in compile-time assertions,
  not in code review.

### BUG-12 (Critical) — every transport failure collapsed into one word

- **Symptom**: entering a play ID on the device showed "unavailable" for
  every play across many firmware versions, while the same plays analyzed
  fine from the host. Server-side fixes (404 coverage, warn-and-allow
  policy) never cured it, because the device's own transport failures were
  never visible.
- **Root cause**: `meta_store_api_analyze` wrote the literal "unavailable"
  into `reason` for *every* failure path — TLS handshake failure, DNS
  failure, read timeout, non-200 status, response over the buffer limit.
  The one that matters on real hardware: with SNTP unsynced, `time()` stays
  near 1970 and **mbedTLS certificate time validation always fails**, so
  HTTPS can never succeed — indistinguishable on screen from "server is
  down". Also found on the pass: the over-limit path skipped the reason
  assignment entirely (stale reason from a previous analysis could be
  shown).
- **Fix**: transport failures classified by **stage x clock state**
  (`meta_store_api_fail`, pure logic, host-tested): `TLS failed (clock
  unsynced).` / `TLS/DNS failed. Retry.` / `No response. Retry.` /
  `Connection lost. Retry.` / `Bad response from server.` /
  `Server error <code>`; business reason codes (not-found/format/too-large)
  pass through untouched; P2 renders transport texts verbatim with a
  RETRY row instead of wrapping them in "Not supported:". reason buffer
  widened 24→40 for the sentence forms.
- **Lesson**: a single catch-all error string is a diagnostic black hole —
  every failure layer the device can distinguish, it must surface; and
  "works from the host" proves the server, never the device's TLS/clock
  stack (the host never runs either).

### BUG-13 (Critical) — the device never had a TLS trust anchor

- **Symptom**: v25 on device: entering a play id shows `TLS/DNS failed.
  Retry.` for every play, while the same site analyzes fine from the host
  and passes all 15 production E2E checks.
- **Root cause, two layers**: ① `esp_http_client_config_t` never attached
  any certificate source — no `crt_bundle_attach`, no `cert_pem` — so the
  mbedTLS session had zero trust anchors and every handshake failed no
  matter what the network was doing (SNTP worked, proving DNS and routing
  were fine, which is what made the classifier's stage attribution
  trustworthy enough to look at the TLS layer specifically). ② The
  bundled `main/certs/gtsr4.pem` was the **cross-signed** GTS Root R4
  (issuer = GlobalSign Root CA), unusable as an mbedTLS trust anchor: the
  anchor itself chains to a missing issuer (`unable to get issuer
  certificate`), reproduced on host with `openssl s_client -CAfile
  main/certs/gtsr4.pem` returning code 2. A cross-signed root only works
  when its issuer is also present; a device bundle ships nothing else.
- **Fix**: replaced with the self-signed GTS Root R4 (issuer == subject,
  fp `71:CC:A5:39:…:A4:BD`); both analyze and install configs attach
  `esp_crt_bundle_attach` (main/certs bundle, `esp-tls` in REQUIRES).
  Verified end-to-end on host: `openssl s_client` with the device anchor
  against the production site returns **code 0 (ok)**.
- **Permanent gates**: E2E-8 runs the full production handshake against
  the device's actual anchor bundle — this is the test whose absence let
  the chain ship broken through five firmware versions; E2E-8b rejects
  any cross-signed anchor in the bundle (subject != issuer).
- **Lesson**: "the E2E checks pass" must include the device's real crypto
  material — issuer-string matching (old E2E-6) is not chain verification.
  Any trust-anchor change must be validated with a full handshake against
  the production endpoint, exactly the way the device will do it.

### BUG-14 (Critical) — list page dead after one store visit (s_keys[10] overflow)

- **Symptom**: after visiting the store once — which always builds P1, since ONLINE
  auto-advances into the id-entry page 2s after connecting — the slot list page could no
  longer highlight STORE DOWNLOAD, and keys stopped doing anything. A fresh boot worked
  until the store was entered.
- **Root cause, proven from the linker map**: `s_keys` was still declared
  `static lv_obj_t *s_keys[10]` — the r8 keypad had 10 keys. r10 grew the keypad to 15
  (`MPD_KEY_COUNT`) and the build loop kept indexing the same hardcoded array: every P1
  build wrote `s_keys[0..14]`, overflowing 5 pointers (40 bytes of .bss). The map placed
  exactly the victims there: `.bss.s_keys 0x3fc999e0 0x28` is followed by
  `.bss.s_rows 0x3fc99a08 0x10` (the four list-page row pointers) and the head of
  `.bss.s_slots`. After leaving the store the screen objects are destroyed, so
  `list_refresh`/`rows_refresh` dereferenced dangling `lv_obj_t*` written by the keypad
  builder — list-page highlight and key dispatch corrupted, at the mercy of heap reuse.
  The only trace in the build log: `warning: iteration 10 invokes undefined behavior`
  (`-Waggressive-loop-optimizations`, main.c:495 and :514) — sitting there since r10
  while the UB ate the list page.
- **Fix**: the declaration is now `static lv_obj_t *s_keys[MPD_KEY_COUNT]` — array size
  derives from the same source as the build loop, so the keypad can never outgrow it
  silently again; both UB warnings are gone from a clean build.
- **Lesson**: a hardcoded size next to a loop bounded by a different constant is a
  time bomb no device test covers; the compiler flagged it from day one. Firmware-build
  warnings must be treated as failures, not grepped away — `-Werror` now applies to the
  main component.

### Wording (r10, not a bug but a contract) — slot states speak plainly

`(invalid)` was ambiguous (it suggested a broken device/slot) for what is
usually ota_2 holding littlefs recordings — user data, not corruption. The
states now read `(empty)` (erased) and `(no firmware)` (data present, not a
bootable image), both installable over directly (esp_ota_begin erases first);
wording lives in `meta_slot_list_word`/`meta_slot_detail_word`, pinned by
host tests so it cannot drift silently.

### Process lessons (r9–r10.6)

1. **E2E must target the production site** — local server.mjs proves the
   code, not the deployment. `tools/e2e-production.mjs` is the
   acceptance gate for the data plane; re-run it before any device flash.
2. **Device-only paths get static review + screen diagnostics** — RF, task
   lifecycles, and UI state machines cannot be host-tested; each F1–F4
   finding came from a checklist against known IDF failure modes, and every
   remaining failure now prints its layer and reason code on screen.
3. **Extract device decision logic into pure modules** (`meta_store_idedit`,
   `meta_store_prov`, `meta_store_analysis`, `meta_store_api_fail`) so the
   host tests run the same code the device runs — "verified on the
   simulator" claims died here twice before this rule was adopted.
4. **Design the page-flow graph before wiring keys** (BUG-10/11) — the
   inescapable P0↔P1 loop and the dead short-press navigation both came
   from writing key handlers with no transition model on paper. The flow
   graph (plan §8 r10) is now the canonical spec; every transition exists
   there before code does.
5. **No failure may collapse into one word** (BUG-12, r10.4) — "unavailable"
   told the user and the developer nothing for five versions. Device
   transport failures now classify by stage × clock state; every server
   error path carries a `detail` with the upstream status/phase. Debuggability
   is a contract on both sides, not a nice-to-have.
6. **"E2E passes" must include the device's real materials and request
   surface** (BUG-13) — issuer-string matching is not chain verification,
   and a host fetch is not the device's HTTP stack. E2E-8 runs a full TLS
   handshake with the device's actual anchor bundle; contract checks replay
   the device's exact request surface (HTTP/1.1, device UA, no extra
   headers). What the device runs is what gets tested.
7. **Build warnings are failures** (BUG-14) — two `iteration 10 invokes
   undefined behavior` warnings sat in every build log since r10 while the
   UB ate the list page; grepping warnings away to read the PASS lines is
   the process error that let it live. The main component builds with
   `-Werror`; warnings may not be filtered out of attention.
8. **Single-source every array size from the constant that bounds its
   loops** (BUG-14) — `s_keys[10]` next to `for (i < MPD_KEY_COUNT)` was a
   bomb no device test covers and no code review caught for three versions;
   the declaration and the loop must derive from one constant.
9. **Test tooling must separate environment failure from service failure**
   (2026-09-28 egress incident) — a local proxy TUN hijacked DNS to fake-ip
   and reset TLS, a symptom shape identical to a production outage; hours
   went to "is the service down" before the layer was found. E2E-0 now
   prefights the egress: fake-ip + failed handshake exits 2 (environment),
   never misreported as service failure (exit 1).
10. **Release artifacts come from clean rebuilds** (v26–v28 Kconfig drift) —
   incremental builds carried a stale full Mozilla cert bundle through
   three versions while sdkconfig.defaults claimed otherwise. `rm -rf build`
   before every release artifact, then verify the effective sdkconfig (and
   the artifact itself), never assume.

---

## PASS-RADAR "still unsigned" — root cause and resolution

Symptom reported on-device: the pass-radar firmware signed with the *fixed*
signing tool still shows "Unsigned firmware!" at boot.

Investigation (all checks reproducible on this host):

1. **The signing chain itself is correct.**
   `tools/signing/run-verify-tests.sh` compiles the *real* firmware verifier
   (`main/meta_sign.c`, no `HOST_TEST`) against the embedded
   `meta_sign_pubkey.h` and reports `META_SIG_OK` on the newest signed image
   (`../pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin`, 19:09).
   The pre-fix image (`pass-radar_signed_v2.bin`, 13:53) correctly fails with
   `META_SIG_VERIFY_FAIL` — it was signed with the broken digest and must be
   re-signed. Launcher builds in `build/` (12:07 and 19:06) both embed the
   current public key, so launcher and signer agree.
2. **The real bug was the install path: BUG-03.** The README's primary local
   install workflow is `node tools/install-slot/server.mjs` → the dev copy of
   the installer. That copy (a) wrote the display-name blob with a second
   `writeFlash` into the same 4 KB tail sector that already held the MSIG
   signature, erasing it, and (b) computed the blob address from the
   partition size, flashing outside the slot. Installing through it destroys
   the signature sector → the launcher sees an erased (0xFF) tail → "Unsigned
   firmware!" — even though the .bin file itself was signed correctly.

**Conclusion:** re-sign with the current `sign-firmware.sh`, then install via
the (now fixed) dev page or the Cloudflare Pages page — do **not** re-install
from an artifact previously written by the old dev page without re-writing
the tail sector. BUG-03's fix closes the on-device reproduction path.

**Re-sign verified (follow-up):** `pass-radar.bin` (962416 B) was confirmed
to be byte-identical prefix of the latest signed image, then re-signed fresh
with the current `tools/signing/sign-firmware.sh` (image_len 962416,
sig_offset 962560, payload_len 70): the firmware verifier reports
`META_SIG_OK` on both the fresh artifact and the existing
`pass-radar_v0.1-2-g8fcce59-signed.bin`. The old `pass-radar_signed_v2.bin`
remains invalid by design and must not be installed.

**Local flasher verified end-to-end (follow-up 2):** the local install
service (`node tools/install-slot/server.mjs`, canonical `install-slot/` page)
was smoke-tested (page, ES modules, vendor bundles all 200; SSRF guard and
404 behave), and the full install byte-path was replayed against the real
signature: `extractAppImage()` on the signed image yields image_len 962416 /
tail offset 0xEB000 / full 4 KB tail sector; after applying the page's MNAM
display-name patch (right-aligned at 4056), the assembled slot bytes pass
the *firmware* verifier (`main/meta_sign.c` + real mbedtls): `META_SIG_OK`,
`meta_sign_detect_sector() == true`, egg text intact. The installer's
`tailSectorOffset` (4K-aligned after image_len) matches the device-side
`meta_sign_sector_offset()` and the design doc §7 layout exactly
(MSIG [0..127] / MAEG [128..4055] / MNAM [4056..4095], single-write sector).

Verification commands:

```bash
tools/signing/run-verify-tests.sh ../pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin
node tools/install-slot/test-extract.mjs   # installer unit tests (canonical modules)
bash tools/validate.sh --static            # static checks + host tests, all PASS
PORT=4191 node tools/install-slot/server.mjs  # local flasher; open http://localhost:4191/
```

---

## BUG-01 (High) — `add_battery()` renders a garbage battery label

**File:** `main/main.c:64-71`
**Status: FIXED** — the exact fix below is applied (`soc < 0` guard at
`main.c:69`); code block kept as the historical defect.

```c
static void add_battery(lv_obj_t *parent)
{
    const int soc = bsp_battery_soc();
    char text[12];
    lv_obj_t *lbl = ui_pixel_label(parent, text, &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(lbl, 204, 30);
}
```

**Symptom.** A label is created from `text`, a **completely uninitialized stack
buffer**. `ui_pixel_label()` immediately calls `lv_label_set_text(label, text)`
(`ui_pixel.c:18-26`), which `strlen()`s the buffer: it draws whatever bytes
happen to be on the stack, and can even read past the 12-byte array when no
NUL is present. The comment promises "when reading −1 (unavailable), don't
draw, to avoid showing a fake number" — and `bsp_battery_soc()` does return −1
on failure (`bsp_battery.h:13`) — but the value is never used. `soc` is dead,
which also means the firmware build carries an unused-variable warning nobody
acted on (the host test suite in `tools/validate.sh` never compiles `main.c`,
so `-Werror` never sees it).

**Root cause.** Half-finished feature: the SOC → text formatting and the
"don't draw when unavailable" guard were never written. Affected on both call
sites — the list screen (`main.c:143`) and the detail screen (`main.c:308`) —
i.e. the two most-used pages show garbage in the top-right corner.

**Fix.**

```c
static void add_battery(lv_obj_t *parent)
{
    const int soc = bsp_battery_soc();
    if (soc < 0) return;                       // no gauge → don't draw
    char text[12];
    snprintf(text, sizeof(text), "%d%%", soc);
    lv_obj_t *lbl = ui_pixel_label(parent, text, &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(lbl, 204, 30);
}
```

(Ensure `<stdio.h>` is included for `snprintf`.)

---

## BUG-02 (High) — Inverted egg-magic check in the `HOST_TEST` parser

**File:** `main/meta_sign.c:47` (`#ifdef HOST_TEST` variant of `meta_egg_parse`)
**Status: FIXED** — the stray `!` was removed upstream; the line now reads
`if (memcmp(egg, egg_magic, 4) != 0) return META_EGG_ABSENT;` and matches the
device variant and host stub. Code block kept as the historical defect.
**Regression guard added:** `tests/test_meta_net_upload.c` now exercises the
egg path — `m1_egg_parse_valid` (valid MAEG → `META_EGG_OK`),
`m2_egg_parse_absent` (erased sector → `META_EGG_ABSENT`),
`m3_upload_signed_tail_preserved` (signed tail with MAEG survives an upload,
MSIG + egg intact, dispname rejected) and `m4_upload_unsigned_tail_rebuilt`
(unsigned upload rebuilds MNAM, no MAEG residue). The old inverted variant
would fail m1/m3 immediately.

```c
static const unsigned char egg_magic[4] = META_EGG_MAGIC_BYTES;
if (!memcmp(egg, egg_magic, 4)) return META_EGG_ABSENT;
```

**Symptom.** The semantics are inverted relative to every other
implementation of the same function:

- device variant, `main/meta_sign.c:97`: `if (!check_egg_magic(egg)) return META_EGG_ABSENT;` (absent = magic *missing*)
- host stub, `tests/esp_stubs/meta_sign_stub.c:58`: `if (memcmp(egg, egg_magic, 4) != 0) return META_EGG_ABSENT;`
- format tests, `tests/test_meta_sign.c:158`: "erased sector (no MAEG) → `META_EGG_ABSENT`"

The HOST_TEST variant returns `META_EGG_ABSENT` when the magic **matches**,
i.e. a valid egg is reported absent, and a blank (0xFF) sector is treated as
"egg present" and pushed into the parse path.

**Root cause.** A stray `!`. Note this is not dead code:
`tools/validate.sh` compiles `tests/test_meta_net_upload.c` from the *real*
`main/meta_sign.c` with `-DHOST_TEST` (validate.sh:73-78), so the variant is
built on every CI run — the upload test just doesn't exercise the egg path
yet, which is why nothing caught it.

**Fix.** Make it match the other two implementations:

```c
if (memcmp(egg, egg_magic, 4) != 0) return META_EGG_ABSENT;
```

Better: delete the HOST_TEST block from `main/meta_sign.c` and have
`test_meta_net_upload` link `tests/esp_stubs/meta_sign_stub.c` for
`meta_egg_parse`/`meta_sign_verify` (as `test_meta_sign` already does), so
there is only one implementation left to keep correct.

---

## BUG-03 (High) — Dev installer copy has drifted into correctness bugs

**Files:** `tools/install-slot/install-slot.html`, `tools/install-slot/extract-app-image.js`
vs. the shipped `install-slot/` copies.
**Status: FIXED** — the shipped single-write tail-sector flow and the
`& 1` bit-test were ported into the dev copy; `diff` reported the copies
identical (modulo the one-line path annotation in `name-blob.js`).
**Structural fix also done (dedupe):** the duplicated dev page files were
deleted; `tools/install-slot/` now contains only `server.mjs` and
`test-extract.mjs`, and `server.mjs` serves the canonical `install-slot/`
directory directly (`PAGE_DIR = ../../install-slot`), i.e. the exact bytes
deployed to Cloudflare Pages. `test-extract.mjs` imports the canonical
modules and reads the canonical HTML for its i18n test. `README.md`,
`README.zh_CN.md` and `install-slot/README(.zh_CN).md` layout sections were
updated accordingly. Verified: `node tools/install-slot/test-extract.mjs`
→ 8/8 PASS; live smoke of `server.mjs` (`/`, `/name-blob.js`,
`/extract-app-image.js`, `/vendor/esptool-js.js` → 200).

The repo keeps two installer copies (README:152-153): `install-slot/` is the
deployed Cloudflare Pages page; `tools/install-slot/` is the local dev copy
served by `server.mjs:91-94`. Nothing checks that they agree, and the dev copy
has fallen behind in three behavior-affecting ways.

### (a) Name blob written outside the slot partition (corrupts the next partition)

Dev copy, `tools/install-slot/install-slot.html:133,561-563`:

```js
import { packNameBlob, sanitizeDisplayName, blobOffset, maxAppImageSize } from "./name-blob.js";
...
const blob = packNameBlob(dispName);
const blobAddr = address + blobOffset(s.size);
```

`blobOffset(len)` = `ceil(len/4096)*4096 + 4056` (`name-blob.js:23-29`) — it
expects the **image length**, but the dev page passes the **partition size**
`s.size`. Since `s.size` is 4K-aligned, this lands the blob at

```
slot_offset + part_size + 4056
```

which is 4056 bytes *past the end of the slot*. Concretely for slot 0
(`ota_0`, 0x180000 + 0x1D6000 = 0x356000 end): the blob is flashed at
**0x356FD8 — inside the `cardid` NVS partition** (0x356000, 0x4000), and for
the other slots it lands at the start of the next app partition. Even under
the old pre-tail-sector layout this is off by a full sector (an "end of
partition" blob would be `slot_offset + part_size − 40`). Result: the display
name silently doesn't work *and the installer corrupts an unrelated partition*
(the device's card ID, or the neighboring slot's first sectors).

The shipped page has the correct, image-length-based single-write flow
(`install-slot/install-slot.html:564-584`, `packNameBlobTail` merged into the
4 KB `image.tailSector` written at `image.tailSectorOffset`).

### (b) Double-write erases the signature/egg sector

The dev page writes the app image first, then issues a **second**
`writeFlash` for the blob (dev page:552-569). The esptool flash write erases
each destination sector before programming, so the second call — targeting
the same 4 KB tail sector the first call already touched — wipes whatever the
first write put there. For a signed image the MNAM window overlaps the MSIG
sector, so **installing via the dev page erases the signature**; the launcher
then shows "Unsigned firmware!" for a trusted build. This is exactly what the
shipped page's comment calls out ("split writes will repeatedly erase the same
sector, wiping the signature/easter egg written first",
`install-slot/install-slot.html:562-563`) — the fix never made it back into
the dev copy.

### (c) `hashAppended` flag detection drift

`tools/install-slot/extract-app-image.js:29`:

```js
const hashAppended = buf[start + 23] === 1;              // dev copy
const hashAppended = (buf[start + 23] & 1) === 1;        // shipped copy, line 29
```

Byte 23 of the ESP image header is a flags byte (bit0 = hash_appended). Today
ESP-IDF writes exactly 0 or 1 so both behave identically, but the dev copy's
equality test miscomputes the image length (32 B short) for any image with
another bit set in that byte, truncating the app image written to flash.
The dev copy should adopt the shipped bit-test.

**Root cause.** Copy duplication without a sync mechanism: `tools/check_repo.py`
doesn't compare the copies, `tools/install-slot/test-extract.mjs` tests the dev
`extract-app-image.js` but not against the shipped one, and the dev HTML simply
wasn't updated when the tail-sector write landed.

**Fix.**
1. Short term: port the shipped page's write flow into
   `tools/install-slot/install-slot.html` and the bit-test into its
   `extract-app-image.js`.
2. Structural: make `tools/install-slot/server.mjs` serve the canonical
   `install-slot/` directory (single source of truth), or add a drift check to
   `tools/check_repo.py` / CI asserting the copies are identical modulo the
   one-line header comments.

---

## BUG-04 (Low) — `%d` for a `size_t` value in the upload log

**File:** `main/meta_net.c:407`
**Status: FIXED** — now `%zu` with the `size_t` argument directly.

```c
ESP_LOGI(TAG, "slot %d written: %s %s (%d B)", slot, name, ver, req->content_len);
```

`req->content_len` is `size_t` (ESP-IDF `httpd_req_t`; the project's own stub
types it so at `tests/esp_stubs/esp_http_server.h:32`). Harmless on the
32-bit ESP target, but a latent `-Wformat` warning on other hosts and
inconsistent with the `%u`-style handling elsewhere. Use
`(unsigned)req->content_len` with `%u`. Cosmetic.

---

## BUG-05 (High) — Periodic WiFi scan while the SoftAP is up makes the hotspot invisible

**File:** `main/meta_store_net.c` (r7 provisioning scan)
**Status: FIXED in r8** — scan-on-demand (`s_scan_req` + `PROV_SCAN_WAIT_MS` 2.5s
wait in the `/api/scan` handler; one scan per request, no periodic timer).

Symptom on real hardware: with the provisioning hotspot up, phones could not
see `metapass-XXXX` in their network list at all — while the same radio had no
trouble joining the saved home network moments earlier.

Root cause: r7 scanned every 3s from a background task to keep the provisioning
page's network list fresh. `esp_wifi_scan_start` must hop the radio to each
channel to listen for beacons, and while it does so the SoftAP stops
transmitting its own. A 3s cycle means the beacon is dark a large fraction of
every cycle; phone scans (themselves a few seconds apart) kept landing in the
gaps. On the bench this read as "page loads fine" because the QEMU/simulator
covers only the HTTP layer — the RF interaction is invisible to every host
test.

Lesson: **anything on the air shares one radio.** A co-located periodic RF
operation (scan, BLE activity, SNMP-ish polling) must be treated as a suspect
when an AP "isn't visible" — and AP-visible-ness can only be verified with a
real phone against real hardware. Event-driven, on-request execution is the
default; "periodic refresh" for UI convenience is not free.

## BUG-06 (High) — Fixed six-digit zero-padded play ID did not match the market's ID space

**File:** `main/main.c` (P1 ID entry, r7 and earlier)
**Status: FIXED in r8** — P1 rebuilt as an on-screen keypad with variable-length
IDs (1–7 digits, `ID_MAX_DIGITS`; the 7th digit auto-commits), matching the
server's `\d{1,7}` id validation.

Symptom: entering a short play ID on the numeric page could only produce exactly
six characters; anything shorter had to be imagined with leading zeros, and a
5-digit play like 563 was awkward while 4-digit IDs were simply impossible.

Root cause: the entry UI was built around a fixed-width format that exists
nowhere in the product it talks to — the market identifies plays by
variable-length numeric IDs (`\d{1,7}` server-side). The fixed width wasn't a
tightened validation, it was an invented constraint.

Lesson: **input formats must mirror the authoritative ID space, not a UI
convenience.** When a display can't show an input field, the on-screen keypad is
the fix — not a reinterpretation of the user's data. Zero-padding a numeric ID
silently addresses a *different* resource (01 ≠ 1 on this market), so it was a
correctness bug, not a cosmetic one.

---

## Investigated and cleared (evidence)

- **LVGL 9.5 timer self-delete in UI teardown paths** — `lv_timer.c` guards
  callbacks with `act_timer_deleted`; deleting one's own timer from its
  callback is supported. Not an issue.
- **Programmatic scrolling of the egg panel** — `block()` strips
  `LV_OBJ_FLAG_SCROLLABLE` from its own children, but the egg panel
  (`main.c:189-190`) re-enables scrolling via `lv_obj_set_scroll_dir()`;
  `lv_obj_scroll_by_raw()` doesn't check the flag at all. Scroll-by direction
  semantics match the LVGL 9.5 header contract (`dy > 0` moves toward
  top/beginning), so `btn == BSP_BTN_UP ? +step : −step` (`main.c:440`) is the
  correct mapping.
- **Size-limit arithmetic** — for the 4K-aligned partition sizes in
  `partitions.csv` (0x1D6000 / 0x200000 / 0x29E000) the bound
  `part_size − 4096` used by `meta_sign_app_limit()` (`meta_sign.h:56`),
  `meta_name_max_app_size()` (`meta_name.h:42`) and JS `maxAppImageSize()`
  (`name-blob.js:33`) is *exactly* tight: for any accepted `image_len`,
  `ceil(image_len/4096)*4096 ≤ part_size − 4096`, so the tail metadata sector
  always fits. No off-by-one.
- **Button long-press wiring** — `bsp_button.c:72-76` registers
  `BUTTON_LONG_PRESS_START` with `press_time = BSP_BTN_LONG_MS (1500)` in a
  stack-local `button_event_args_t`; `iot_button_register_cb` copies
  `press_time` into its internal cb_info array before returning
  (`iot_button.c:378,402-411`), so the stack lifetime is fine. The zeroed
  `button_config_t` makes `TIME_TO_TICKS(0, LONG_TICKS)` fall back to
  `CONFIG_BUTTON_LONG_PRESS_TIME_MS` (Kconfig default 1500 ms), consistent
  with the explicit args.
- **`meta_seq` matcher** — traced all paths (gap timeout at index > 0, key
  mismatch with restart, repeated first key, wraparound via unsigned
  subtraction): behavior matches the design doc.
- **`meta_name_pack_tail`/`unpack_tail` ↔ JS `packNameBlobTail`** — right-
  aligned 40 B window, xor last byte; C and JS agree byte-for-byte.
- **`meta_egg_parse` window bounds** — compile-time checked
  (`META_EGG_WINDOW_OFF + META_EGG_TOTAL_LEN ≤ META_NAME_BLOB_OFF` =
  128 + 3928 ≤ 4056) and match `sign-firmware.sh`'s layout (MSIG@0, MAEG@128,
  MNAM@4056).
- **`image_len` sourcing** — upload path uses `esp_image_verify` metadata,
  correctly avoiding the trap of reading the header's non-length field at
  +20 (`meta_net.c:314-318`).
- **Shipped installer tail-sector extraction** — extracts MSIG/MAEG/MNAM into
  one `image.tailSector` and writes it in a single `writeFlash`; correct.

---

*Report generated from static review + host-test source verification. Line
numbers refer to the current branch. Companion file:
[BUGS.zh_CN.md](BUGS.zh_CN.md).*
