English | [简体中文](meta-pass-network-download-plan.zh_CN.md)

# meta-pass Store Channel Plan v3.2 — Server-Side Unpacking + Numeric-ID Direct Access (Current Scope)

> Baseline: GitHub `alexwwang/meta-pass` main branch (v1.0.0, commit `e08dc4d`)
> Server: metapass.chuanxilu.net (existing hosting + this extension)
> Firmware sample: FoloToy AI Passport app store (plays #563, slug `ai-passport-9`, revision 1279-4)
> Date: 2026-09-24

---

## 0. Scope of This Phase (v3.2 Convergence)

**What we build (the only delivery chain this phase)**:

```
User sees an interesting play on the official marketplace web/App → remembers the play number (ID)
  → device STORE: enter the ID → one call to /api/analyze returns {name, can it be installed, smallest fitting slot}
  → confirm install → slot selection (default = smallest fitting slot) → OTA download of the separated firmware → flash into the chosen slot
  → prompt to reboot → launcher menu boots the child firmware
```

**Information-access constraint (v3.2 revision)**: the device **never pulls store details locally** —
zero calls to store detail endpoints. The three pieces of information (app name, installability,
smallest fitting slot) are obtained **only via a single request to metapass.chuanxilu.net's
`/api/analyze`**; afterwards the UI offers only an "INSTALL" confirm button leading to the next
step. The name comes from the server as an ASCII-safe value (the slug), which also solves the
constraints that the device screen has no CJK glyphs and MNAM only accepts ASCII.

**What we do NOT build (explicitly deferred; research conclusions in Appendix A, endpoint facts
confirmed and directly usable in later iterations)**: on-device marketplace login, favorites,
category-list-detail browsing. Rationale: login interaction on the device is too costly; this
phase prioritizes the core "OTA download into slot" feature. Browsing stays on the official
marketplace web/App; the device only knows numbers.

**Architecture decisions (inherited from v3, unchanged)**:
- Unpacking/separating is **not done in firmware**; metapass.chuanxilu.net does it server-side
  (reusing the same hosted implementation that is byte-identical to the repo's
  `install-slot/extract-app-image.js`; the Node side imports the same ES module);
- The device **talks only to metapass.chuanxilu.net** (single TLS trust anchor), which proxies the store;
- Firmware only does: provisioning → numeric play-ID entry → analyze (name / installability /
  smallest slot) → confirm → slot selection → download pure app image → OTA write; the device
  makes zero calls to store detail endpoints.

```
┌──────────────────────┐         ┌──────────────────────────┐         ┌─────────────────────────┐
│  AI Passport device   │  HTTPS  │  metapass.chuanxilu.net   │  HTTP   │  ai-passport.folotoy.cn  │
│  meta-pass STORE fw   │ ◄────► │  (server.mjs extension)   │ ◄────► │  (store API + firmware)  │
│  ID/detail/slot/dl    │         │  proxy + unpack + cache   │         │                          │
└──────────────────────┘         └──────────────────────────┘         └─────────────────────────┘
```

---

## 1. Server Design (metapass.chuanxilu.net Extension)

### 1.1 Measured Status (2026-09-24)

| Endpoint | Status | Notes |
| --- | --- | --- |
| `GET /api/play?id=<number>` | 200 | Store detail proxy (**numeric IDs only**, slug → 400) |
| `GET /api/firmware?path=<store path>` | 200 | Firmware byte proxy (path whitelist; measured SHA-256 matches the store) |
| `/extract-app-image.js` and other static files | 200 | Byte-identical to repo `install-slot/` |
| Unpack analysis / separated-firmware endpoints | 404 | **Added this phase** |

### 1.2 Endpoint Contracts (2 New Endpoints This Phase)

#### `GET /api/analyze?id=<number>` — Unpack Analysis + Slot Suggestion

Flow: fetch detail → check `ok` / `firmware.available` / `format` → download merged image (cached) →
**verify against the store-published `firmware.sha256`** → unpack with `extract-app-image.js` →
obtain exact app-image length and SHA-256 → compare against the slot geometry table → respond:

```json
{
  "ok": true,
  "id": 563, "revisionId": 1279,
  "name": "ai-passport-9",
  "store":  { "size": 3219456, "sha256": "0d68...c70a" },
  "extracted": { "imageLen": 1844496, "sha256": "<sha256 of unpacked image>" },
  "slots": [
    { "slot": 0, "limit": 1921024, "fit": true },
    { "slot": 1, "limit": 2093056, "fit": true },
    { "slot": 2, "limit": 2740224, "fit": true }
  ],
  "suggestedSlot": 0,
  "supported": true, "reason": "ok"
}
```

(HTTP status: business results are always 200 + the JSON above — including `supported=false`
reason branches; the device does not read the body of non-200 responses, and expressing business
reasons via 4xx would flatten real reasons into "unavailable".)

- `name`: **the app name (ASCII-safe, taken from the slug)**, displayed by the device and written
  into the MNAM display-name blob; the device therefore never needs store details, and the CJK
  problem of title.zh is eliminated server-side;
- When `supported=false`, `suggestedSlot=-1` and `reason` is one of: `not-found` (ID does not
  exist) / `unavailable` (delisted) / `format` (not esp-merged-0x0) / `no-factory` (no factory
  app in the partition table) / `wrong-chip` (not ESP32-C3) / `custom-partitions` (custom data
  partitions outside the whitelist) / `too-large` (exceeds the largest slot). Decision rules match
  the `extract-app-image.js` error branches + store firmware semantics; the server reuses them directly.

Slot geometry table (server-side constant, kept in sync with `partitions.csv` /
`meta_sign_app_limit`; app limit = partition size − 0x1000 for the 4KB tail metadata sector):
slot0=0x1D5000(1,921,024B), slot1=0x1FF000(2,093,056B), slot2=0x29D000(2,740,224B).
**Suggested slot = the smallest slot that fits**; none fits → `too-large`.

#### `GET /api/extracted?id=<number>` — Returns the Separated Pure App Image

- Reuses analyze's cached result, `application/octet-stream`, `Content-Length = imageLen`,
  response headers include `X-Image-Len` / `X-SHA256` (matching analyze's `extracted.sha256`);
- First version does not support HTTP Range; device-side retry = full re-download (≤3 attempts);
- Implementation note: cache only `{imageLen, sha256}` + the merged image bytes (LRU, 512MB
  default); when an extracted request arrives, unpack on the fly and stream out (trade CPU for disk).

#### Caching Policy

- Detail proxy: the store marks `no-store` → do not cache;
- analyze: key=`id:revisionId`; on hit, still re-origin to confirm revisionId is unchanged;
- extracted: derived from the analyze cache, not stored separately.

#### Server Security Constraints

- `id` numeric validation against enumeration/SSRF; upstream fixed to `https://ai-passport.folotoy.cn`;
- merged-image download cap 8MB; per-IP rate limiting on analyze/extracted;
- extracted must come from unpacking a "SHA-256-verified merged image"; streaming unverified
  bytes while downloading is forbidden.

### 1.3 Cloudflare Pages Deployment Feasibility (v3.2-r3 Measured)

Current state: the site is hosted on Cloudflare Pages (static assets + Pages Functions serving the
existing `/api/*` proxies; local dev counterpart is `tools/install-slot/server.mjs`). Checks for
this phase's requirements:

| Requirement | Pages/Workers support | Approach |
| --- | --- | --- |
| `/api/analyze` (origin fetch 3MB + SHA-256 + pure-JS unpack) | ✅ all compatible | The unpack chain `extract-app-image.js → name-blob.js` is **zero Node API, pure ESM** (import chain verified); Functions can `import` directly; SHA-256 via WebCrypto `subtle.digest` (native, milliseconds); 3MB `fetch` + streaming ≈ 10MB memory, far below the 128MB limit |
| `/api/extracted` (~2.7MB byte-stream response) | ✅ | Workers streaming responses; on R2 hit, pipe `R2Object.body` directly |
| Cache (no filesystem) | ✅ swap storage | **R2** (strongly consistent; merged image + analyze JSON; 10GB free tier is enough for a 512MB LRU) + **KV** (rate-limit counting only; KV has ~60s global eventual consistency — do not use for strongly consistent reads like analyze results) |
| Rate limiting | ✅ | KV token bucket |
| Free-tier limits | ⚠️ two items | ① 50ms CPU/request: cold analyze (download+hash+unpack) needs load testing; upgrade to paid ($5/mo, 30s CPU) if exceeded; ② 100k requests/day: device volume is far below this |
| Functions bundle size | ✅ | ~3MB limit; unpack modules are 6KB |

**Conclusion: no need to move off Cloudflare Pages.** Implementation conventions: keep Functions
thin; put all logic in ESM modules (same dual-end reuse pattern as `extract-app-image.js`);
align local behavior with `wrangler pages dev`; R2 lifecycle rules for eviction.

### 1.4 Trust Chain

```
Store-published merged sha256 ──► server downloads merged and verifies ──► unpack ──► extracted sha256
Device ◄── {extracted sha256} + {byte stream} over the same TLS session ──► streamed SHA-256 enforced comparison
```

Hash binding is done server-side on the already-verified merged image; the device verifies
reception integrity. Residual risk = chuanxilu itself compromised, replacing both bytes and
hashes (TLS + host security carry that). The USB channel remains as a side door.

---

## 2. Firmware Design (meta-pass STORE Module)

### 2.1 Module Layout and File Changes

| Change | Notes |
| --- | --- |
| `+ main/meta_store_net.c/h` | WiFi STA + SNTP + HTTP client + job queue + fetch task + status snapshot poll (UI-facing interface isomorphic to `meta_net`) |
| `+ main/meta_store_api.c/h` | chuanxilu API client: bounded JSON extractor (no cJSON), play/analyze/extracted calls, error mapping |
| `± main/meta_import.h` | State machine event renaming/reuse (NET_READY / FETCH_META / RECEIVING / VERIFYING / DONE / ERROR) |
| `± main/main.c` | Import page replaced by STORE page group: P0 provisioning → P1 enter ID → P2 detail → P3 slot → P4 progress → P5 done |
| `− main/meta_net.c/h` | Delete (AP provisioning web logic merged into meta_store_net, keeping only the ssid/password form) |
| `main/CMakeLists.txt` | `REQUIRES` gains `esp_http_client esp-tls nvs_flash esp_wifi`; ESP-IDF **v5.5.3** baseline; regenerate `dependencies.lock` after changes |
| `sdkconfig.defaults` | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (anchor GTS Root R4; hardcoding the server certificate is forbidden) |
| **Firmware contains no unpacking logic** | Non-goal |

### 2.2 Page Flow (Six Pages This Phase)

**P0 Provisioning (NETWORK)**
- On entering STORE, check NVS for existing STA credentials (`esp_wifi_get_config`; storage must
  default to flash — `meta_net`'s `WIFI_STORAGE_RAM` does not carry over) → connect if present;
- No credentials: SoftAP + minimal web form collecting `ssid/password` (reusing meta_net's AP
  configuration), save then close AP and switch to STA; Captive Portal auto-popup (DNS hijack)
  is a fast-follow;
- Got IP → **SNTP sync** (3–5s, failure only logged) → P1.

**P1 ID Entry (ENTER ID)**
- 3–6 digit keypad (up/down to change digit, OK to advance), with entered-digit display and
  backspace (OK LONG = backspace/clear);
- On confirm, **one** `GET /api/analyze?id=` (the device's only source of store info) → P2.

**P2 Detail (DETAIL)**
- Rendered entirely from the analyze response, **zero local store detail pulls**:
  - `supported=true` → `<name>\nIMG <unpacked size>  MIN SLOT <suggested slot>` + `INSTALL` confirm item;
  - `supported=false` → `<name|id>\nNOT SUPPORTED` + short reason (`TOO LARGE` / `NEEDS DATA PART` /
    `UNAVAILABLE` / `NOT FOUND`…);
- The page's only action: OK = confirm install (→ P3); OK LONG = back to P1;
- analyze request failure → `FETCH FAILED` + retry in place.

**P3 Slot Selection (SLOT SELECT)**
- Three slot rows: EMPTY / installed name+size; **suggested slot highlighted by default**;
- If slot 2 is selected, has no valid image, but the partition is not all 0xFF (littlefs recording
  storage, design doc §6.3) → append `WIPES RECORDINGS` confirm text;
- OK → P4; OK LONG → back to P2.

**P4 Download (DOWNLOAD)**
- `GET /api/extracted?id=` streamed (dedicated fetch task; UI locked, only OK LONG cancels);
- `Content-Length` / `X-Image-Len` mismatching analyze's `imageLen` → immediate error;
- `esp_ota_begin(part, imageLen)` → 1–2KB chunked `esp_ota_write`, accumulating SHA-256 while streaming;
- After receiving: forced comparison with `extracted.sha256` → `esp_ota_end` → `esp_image_verify` →
  `meta_slot_set_valid` + MNAM display-name blob (from analyze's `name`, guaranteed ASCII server-side);
- Retry = full re-download (≤3 attempts, backoff 1s/2s/4s); cancel/failure = `esp_ota_abort` + mark invalid.

**P5 Done (DONE)**
- `Installed: <name> -> slot N` + `OK = reboot`; OK → `esp_restart()`;
- After reboot, back to the launcher list; the user boots the child firmware from the menu
  (existing flow, including the unsigned warning page).

### 2.3 Network Layer Conventions

- **Threading**: fetch task (stack ≥8KB) runs blocking TLS; UI polls snapshots; buttons send
  START/CANCEL via queue; navigation locked during RECEIVING, only OK LONG cancels;
- **WiFi STA flow**: STA netif → `esp_wifi_get_config` for existing credentials → event group wait
  for `IP_EVENT_GOT_IP` (10s×3) → mid-way `STA_DISCONNECTED` judged BROKEN and retried; 2.4GHz only, UI hint;
- **SNTP**: sync right after IP (`MBEDTLS_HAVE_TIME_DATE` on by default; uncorrected cold-start
  clock can hit `BADCERT_FUTURE`);
- **HTTP**: serial client instances, cleanup after use; `.max_redirect_count=3`; non-200 retried
  as BROKEN; 10s zero-progress watchdog;
- **Battery**: detail page prompts to connect USB when `bsp_battery_soc() < 20%` (real-device calibrated);
- **Session start/stop**: leaving STORE fully `cleanup → wifi_stop → netif_destroy`; connections
  kept while inside STORE.

### 2.4 Firmware Size Budget and Trimming (Keeping the "Single Minimal Firmware" Convention)

Convention unchanged: all new code goes into the factory partition; no firmware splitting, no
second app partition; factory budget 0x170000 = 1,441,792B (<1.43MB, README/design doc §3 constraint).

**Size increment estimate (net over the v1.0.0 baseline)**:

| Component | Estimate | Basis |
| --- | --- | --- |
| mbedTLS TLS protocol layer + X.509 parsing | +40~60KB | Crypto primitives (SHA-256/ECDSA/bignum) **already linked via `meta_sign`** (`main/CMakeLists.txt` already `REQUIRES mbedtls`); only ssl/tls client, x509_crt, ASN1 are new |
| Certificate bundle | +2KB ~ +80KB | **This phase's only device upstream is chuanxilu (anchored at GTS Root R4)**: recommended `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE` with only GTS R4 (reserve ISRG X1 for later direct store access) ≈ +2~3KB; conservative fallback `DEFAULT_CMN` (+15~25KB, whether it contains GTS R4 needs testing) or `DEFAULT_FULL` (+60~80KB) |
| esp_http_client + esp-tls | +15~25KB | Component code + config |
| lwip DHCP client + SNTP | +3~6KB | lwip core already present (SoftAP uses it) |
| WiFi STA mode | +0~5KB | Same WiFi lib as AP |
| New app code (meta_store_net/api + six-page UI + JSON extractor) | +30~50KB | Reference meta_net.c (519 lines) ≈ 20~30KB + pages/UI |
| **Total** | **≈ +90~170KB (custom bundle low end, FULL bundle high end)** | Planned with **~120KB** reserved |

**Baseline headroom**: v1.0.0 actual image size is not recorded in the repo (no binary-size
CHANGELOG entry); **must be re-measured with `idf.py size`** — budget 1.44MB minus current image
= real headroom; based on factory contents (LVGL + audio codec + button + signature verification)
headroom is plausibly several hundred KB, so +120KB likely fits, but "likely" is not "confirmed":
Phase 2 entry gate = run `idf.py size` first, then decide the bundle strategy.

**Trimming levers if over budget (by value)**:
1. Custom two-root bundle (saves 60~80KB; cost: firmware release needed when upstream changes CA;
   mitigation: 2~3 roots in the bundle + USB upgrade as backstop);
2. mbedTLS client-only trimming (disable server suites, disable TLS1.3 as needed, saves ~20~30KB);
3. Reuse existing UI fonts/components, no new glyph sets (CJK cannot enter firmware anyway);
4. Disable unused lwip features.

---

## 3. Trust Model and Verification Strategy

| Stage | Verification | Trust anchor |
| --- | --- | --- |
| Detail/analysis | HTTPS + JSON field whitelist | chuanxilu TLS |
| Unpacking | Server verifies merged sha256 == store-published value before unpacking | Store-published hash → server |
| Firmware landing | Device streamed SHA-256 == analyze.extracted.sha256; `esp_ota_end`/`esp_image_verify` authoritative check; mismatch → erase slot, mark invalid | Hash binding within the same TLS session |
| Malicious firmware surface | Only store-listed plays installable (server-side analysis whitelist rules) | Store review + server rules |
| Side door | USB serial install retained (sole channel for self-built/unlisted) | Physical possession |

## 4. Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| chuanxilu server unavailable | Device prompts retry; USB channel backstop; no bricking (bootloader hook mechanism unchanged) |
| Store API redesign | Server adapts first; device contract stable (analyze/extracted abstract store details) |
| Store download 302 to CDN | Server proxy handles redirects; device contract unchanged |
| Store play built for 3MB, slots smaller | analyze double interception (store.size + unpacked imageLen vs slot limits); refuse before writing flash |
| WiFi disconnect/timeout | 3 connection attempts, full re-download ≤3 (1s/2s/4s), 10s zero-progress watchdog; cancel priority, abort + mark invalid |
| Device clock unsynced | SNTP first (§2.3); failure does not block but is logged |
| TLS handshake OOM | Switch RECEIVING to a static progress page to reduce LVGL usage; verify with `idf.py size` and heap watermarks |
| Low battery brownout | Prompt USB when soc<20% |

## 5. Phased Implementation and Acceptance

1. **Phase 1 — Server**: extend `server.mjs` (analyze, extracted); unpacking directly imports the
   hosted `extract-app-image.js`; Node unit tests use `test-extract.mjs`-style cases + the real
   play 563 firmware as a golden test (**imageLen must equal 1,844,496**); curl-verify each reason
   branch (find a wrong-chip/oversized sample play).
2. **Phase 2 — Firmware network layer**: meta_store_net + meta_store_api + host stub tests
   (`tests/esp_stubs` gains esp_http_client/esp_tls stubs, styled after `test_meta_net_upload.c`).
3. **Phase 3 — UI six pages**: P0–P5 + key dispatch + slot registry write-back; design doc §6/§7
   and README updated in sync.
4. **Phase 4 — Emulator end-to-end**: esp_emu WiFi emulation (`wifi_rx_push`/`wifi_tx_drain`) +
   a Node mock server implementing the chuanxilu contract (stub cert validation or inject a
   self-signed CA), exercising enter-ID→detail→slot→download→list-visible→BOOT end-to-end,
   without real hardware or a router.
5. **Phase 5 — Real-device acceptance**: real-network download of play #563 (suggestedSlot=0,
   imageLen=1,844,496 < slot0 limit 1,921,024), install-reboot-run-return; power-cut during
   download verifies slot invalid and no-brick; low-battery prompt; `idf.py size` re-check of the
   factory 1.43MB budget; SNTP/TLS cold-start measurements.
6. **Regression**: `tools/validate.sh`, emulator cases, `test_meta_net_*` deleted or migrated.

## 6. Key Code Skeletons

```c
// main/meta_store_api.h
typedef struct {
    char     name[48];          // analyze.name (ASCII, from slug; used for display and MNAM)
    uint32_t image_len;         // analyze.extracted.imageLen
    uint8_t  sha256[32];        // analyze.extracted.sha256
    int8_t   suggested_slot;    // analyze.suggestedSlot; -1 = unsupported
    bool     supported;
    char     reason[24];        // TOO LARGE / NEEDS DATA PART / UNAVAILABLE / NOT FOUND ...
} meta_store_analysis_t;

esp_err_t meta_store_api_fetch_analysis(uint32_t play_id, meta_store_analysis_t *out);
// The device's single information entry: GET /api/analyze?id= (slug/available prechecks done server-side)

esp_err_t meta_store_api_download(uint32_t play_id, int slot,
                                  const meta_store_analysis_t *meta,
                                  meta_slot_info_t slots[META_SLOT_COUNT]);
// GET /api/extracted?id= → streamed esp_ota_write + rolling SHA-256
//   → compare meta->sha256 → meta_slot_install_finish() (esp_ota_end/esp_image_verify/MNAM)
```

```js
// server.mjs side (sketch)
import { extractAppImage } from "./extract-app-image.js";
app.get("/api/analyze", async (req, res) => {
  const id = validateId(req.query.id);
  const play = await storeDetail(id);
  const merged = await cachedMerged(play);            // LRU by sha256
  assertSha256(merged, play.firmware.sha256);         // trust chain step one
  const appImg = extractAppImage(merged, Infinity);
  res.json(buildAnalyze(play, { imageLen: appImg.length, sha256: sha256(appImg) },
                        SLOT_GEOMETRY));
});
app.get("/api/extracted", async (req, res) => {
  const { appImg } = await analyzedCache(validateId(req.query.id));
  res.set({ "Content-Length": appImg.length, "X-SHA256": sha256(appImg) });
  res.end(appImg);
});
```

## 7. Appendix: Measurement Records (Used to Author This Plan)

- Store: `/api/plays/id/563` and `/api/plays/ai-passport-9` equivalent; `/api/plays/563` → 404;
  play 563 firmware download SHA-256 matches the published value; ESP32-C3 / IDF v5.5.3 /
  merged-0x0; certificate chain Let's Encrypt YR1 ← ISRG Root YR (X1 cross-signed).
- chuanxilu: `/api/play?id=`, `/api/firmware?path=` usable; unpack endpoints 404 (added this
  phase); certificate chain GTS WE1 ← GTS Root R4 (covered by the mozilla bundle).
- Unpack golden values: play 563 factory app `imageLen=1,844,496B` (segment-table walk +
  %16==15 padding + 1B checksum + 32B hash; tail hash self-verified).

## 8. Revision History

- **v3.2-r10.5 (2026-09-28) TLS trust anchor fixed (BUG-13)**: the on-device failure "every
  play → `TLS/DNS failed. Retry.`" was two defects: ① the HTTP client configs never attached
  any certificate source (zero trust anchors → every handshake fails); ② the bundle's
  GTS Root R4 was the cross-signed variant (issuer=GlobalSign) — invalid as an mbedTLS
  anchor. Fix: self-signed GTS Root R4 in `main/certs`, `crt_bundle_attach` on both analyze
  and install, `esp-tls` dependency. Host-verified: `openssl s_client -CAfile main/certs/gtsr4.pem`
  → code 0. Permanent gates: E2E-8 (full production handshake with the device's real anchor
  bundle — the missing test that let this ship through five versions), E2E-8b (rejects
  cross-signed anchors). E2E count 15→17.
- **v3.2-r10.1 (2026-09-28) P0 change-WiFi intent = double-press UP**: the r10 rule "any
  keypress cancels ONLINE auto-advance" traded one ambiguity for another (a mis-press
  stranded the user on P0). Now a **double-press UP within 600ms** is the only cancel —
  it is also the explicit change-WiFi action (erases credentials, restarts the AP); single
  presses, OK, and DOWN are inert (mis-press safe, auto-advance untouched). The detector is
  pure logic (`meta_prov_upclick_*` in `meta_store_prov`, host-tested: single click inert,
  600ms boundary exact, reset semantics, backwards-clock robustness). P0 status line reads
  `double-UP = change WiFi / hold OK = exit`.
- **v3.2-r10 (2026-09-28) Store page-flow fixed, P1 geometry/navigation corrected, honest slot
  wording**: ① **Page-flow graph (this is the canonical flow now)**:
  `LIST ⇄ STORE(P0) ⇄ ID(P1) ⇄ INFO(P2) ⇄ SLOT(P3) ⇄ DL(P4) → DONE(P5)`; short-press OK
  moves forward (ID entry → analyze → slot select → install), OK LONG exits the store to LIST
  from **every** store page (P0..P5 — the single stable exit), BACK-type transitions
  (P3→P2→P1 via OK LONG on P3/P2, P1 short-circuit) keep the network session alive. The r9
  defect: P1's OK LONG went to P0, and ONLINE auto-advanced P0→P1 after 2s — an inescapable
  loop with a 2s window for the real exit (P0 OK LONG). Now P1 OK LONG = exit store; P0
  auto-advance is **cancelled by any keypress** (user takes over the decision: stay/CHANGE
  WIFI), and P0's UP/DOWN row reads `> CHANGE WIFI (OK=confirm)` / `OK = enter ID entry` —
  an explicit on-screen change-WiFi entry instead of a reset-only affordance. ② **P1 short
  presses fixed**: UP/DOWN short = selection ring ±1 (visible highlight; the r9 mapping to
  cursor moves left/right moved an invisible caret — the keypad was unreachable by short
  presses, only long-press row-wraps worked; regression caught in review, pinned by
  `test_ring_navigation` covering all 15 keys both directions). On-screen ◀▶ keep the
  cursor-move semantics. ③ **Geometry**: ID panel 100→84 (was overlapped by the keypad's
  first row by 10px), OK height 58→64 so its bottom edge aligns exactly with the 0-key
  (both y=274); both invariants are `_Static_assert`ed in main.c — a geometry regression
  now fails the build. ④ **Slot wording**: `(invalid)` → `(no firmware)` as a single source
  of truth (`meta_slot_list_word`/`meta_slot_detail_word`, host-tested in test_meta_slots):
  EMPTY = erased (all 0xFF); NO FIRMWARE = data present but not a bootable image (typical:
  ota_2 dual-use littlefs recordings); both install cleanly over (esp_ota_begin erases
  first) — no wording implying damage or a required delete step. Detail page now says
  `Install overwrites it.` instead of `Delete it and re-install.`
- **v3.2-r9 (2026-09-28) Real-device hardening, policy correction, TDD keypad, production E2E
  gate**: six failure chains from device bring-up, each root-caused and pinned by tests. ①
  Provisioning scan made handler-synchronous with an **unconditional** result drain — a
  finished-but-undrained IDF scan blocks the next scan *and* `esp_wifi_connect` (BUG-05);
  PMF capable; disconnect reasons rendered on screen. ② WiFi lifecycle single-owner (the job
  task): stop/reset are requests the task serializes — no cross-task teardown/start races;
  job stack 6144→8192 B for the mbedTLS peak; the per-second `esp_wifi_connect` fallback
  removed (non-idempotent re-entry = disconnect+reconnect; sticky event bits make it pure
  harm). ③ Session overlay disarmed during AP_UP/CONNECTING/ERROR (it froze the panel while
  the timeout label kept counting); live elapsed-seconds display; SNTP `ntp.aliyun.com`
  primary, 5s cap. ④ ONLINE auto-advance re-anchored to a tick-side state-transition detect
  instead of page-build time (a page built during CONNECTING never advanced). ⑤
  Custom-partitions policy corrected to **warn-and-allow for every non-whitelisted data
  partition** (`detail=<label>`): only the extracted factory app is written, partition
  payloads never enter the device — the r8 hard-reject locked out plays shipping `easter`
  (0x82)/voicefs (0x81) partitions, caught by the production baseline within a day. ⑥ P1 ID
  entry rebuilt TDD as the pure-logic `meta_store_idedit` module (device and tests share one
  implementation): 4×4 grid — 1-9/0, DEL/CLR column, `◀ 0 ▶`, OK spanning two rows — insert
  cursor with backspace, long-press = row wrap, explicit commit; tests caught a digit-mapping
  error before any flash. **Acceptance**: `tools/e2e-production.mjs` — 14 checks against the
  production site (analyze contract, extracted len/sha/magic tri-check, production response
  bytes fed back through the device's C parser, leaf-cert issuer vs the GTS Root R4 anchor,
  server↔firmware reason alignment); `tests/worker_contract.mjs` pins the Pages worker API;
  `validate.sh` all green. Artifacts `meta-pass_v1.0.0-14..18-*.bin`, factory budget ~24%
  free.
- **v3.2-r8 (2026-09-27) Real-device fixes: scan-on-demand, on-screen keypad, reset-WiFi,
  custom-partition warn-and-allow**: four issues surfaced on real hardware or real market
  data. ① **Scan-on-demand**: the r7 periodic 3s background scan runs `esp_wifi_scan_start`,
  which hops the radio off the AP channel for each sweep — the SoftAP beacon develops gaps
  and phones cannot see `metapass-XXXX` at all (beacon hijack). The scan now runs only when
  asked: the `/api/scan` handler sets a request flag and polls up to `PROV_SCAN_WAIT_MS`
  (2.5s) while the network task scans once in the AP_UP state; with no AP, the job loop idles
  at 200ms. ② **P1 ID entry rebuilt as an on-screen keypad**: the fixed six-digit
  zero-padded concept was wrong — market play IDs are variable-length (`\d{1,7}` server-side;
  "nowhere does the market say fixed-width, zero-padded"). P1 renders 0–9 as two key rows
  plus a tall GO key and a CLR bar (UP/DOWN move the 12-key selection with wraparound, OK
  appends a digit, the 7th digit auto-commits via `ID_MAX_DIGITS`, GO commits with ≥1 digit,
  CLR clears; empty shows "ID: -"). ③ **"Reset WiFi" on P0**: saved credentials auto-reconnect
  by design, so a wrong saved network locked the user out of provisioning; the P0 page now
  offers "> RESET WIFI (OK=confirm)" (UP/DOWN toggles it) which calls the new
  `meta_store_net_reset_wifi()` — stop station, erase `sta_ssid`/`sta_pass` from NVS, restart
  the AP. ONLINE still auto-advances to P1 after 2s. ④ **Play 675 "unavailable" root cause**:
  the market entry's merged image contains `rec`, a **data** partition (type=1) with subtype
  0x40 (the ESP-IDF "custom data" subtype, used as a 512KB fallback store), which the analyzer's
  hard-reject rule made uninstallable; the deployed server additionally 404s every
  `/api/analyze` (stale build), and the firmware renders any non-contract reply as
  "unavailable" — the compound visible symptom. Policy change: subtype-0x40 custom data
  partitions are now **warned and allowed** (`supported=true`, reason=custom-partitions,
  `detail=<label>` transmitted in the analyze response), because unpack installs only the
  factory app — the partition's content never reaches the device; hard reject stays for
  type=1 whitelisted-label violations with subtype != 0x40 (the app would look its label up
  at runtime and fail). The analyze contract gains a `detail` string (empty when absent); the
  firmware's interlock check special-cases custom-partitions as dual-state, and the slot page
  renders a two-line note ("NOTE: custom 'rec' part / not installed; some features may lack
  it") while still offering CONFIRM. **Deployment gate**: all of ④ requires redeploying
  `server.mjs` to metapass.chuanxilu.net; until then every play shows "unavailable".
  Factory budget after r8: 1,146,544/1,507,328 B (~24% free). A follow-up contract
  fix in the same cycle: for unsupported plays the server sends `name:null` /
  `extracted:null` (sha256 is computed lazily and `extracted` is null on the error
  path), but the device parser required `name`/`extracted` unconditionally — every
  unsupported reply fell back to "format" on screen. Parsing now requires them only
  when `supported=true`; the analyzer lives in its own translation unit
  (`main/meta_store_analysis.c`) linked by a new host contract test
  (`tests/test_store_analyze_contract.c`) that pins the real response shapes,
  including the unsupported variants and drift rejection.
- **v3.2-r7 (2026-09-27) Provisioning usability: scan list + captive portal**: ① The SoftAP
  hotspot drops its password — an open AP removes the "read the password off the tiny screen,
  type it on the phone" dance; the SSID stays randomized (`metapass-XXXX`) to avoid multi-device
  collisions. ② Scan list: the network task runs `esp_wifi_scan_start` every 3s while the AP is
  up; the provisioning page gains a "Scan networks" button hitting the new `/api/scan` endpoint
  (cached results only, max 20, SSIDs JSON-escaped, results rendered as pick-to-fill buttons via
  textContent — no innerHTML injection surface). ③ Captive portal: a UDP/53 DNS hijack answers
  every A query with the AP gateway IP so phone/PC background probes redirect to the setup page
  automatically (zero typing on systems that auto-open it; others still reach
  `http://192.168.4.1` manually), and a catch-all 302 keeps stray probes on the portal. ④
  `/api/wifi` rejects an empty password (open upstream networks are out of scope; page copy
  matches). Factory budget after r7: 0x1178d0 used, ~24% free.
- **v3.2-r6 (2026-09-25) Cancel authority and timeout configuration**: ① Session timeout changed
  to "ask the user on expiry": no forced network teardown back to the list; an overlay
  "Session timeout. OK = continue / LONG = exit store" freezes other key semantics until the
  user decides (OK = continue current operation and extend / OK LONG = exit; teardown still stops
  the network); before expiry the P0 status line shows "timeout in Ns". ② **Cancellation goes
  through a confirm page** (new P4b, PAGE_STORE_CANCEL): during download, OK LONG enters the
  confirm page while the background download keeps running, with three choices — CANCEL =
  proceed with cancel (`meta_store_api_request_cancel` honored at chunk boundary, half-written
  slot invalidated, failure page shows "Cancelled."); RETRY = cancel the current one and
  automatically re-enqueue the same play/slot install when the job reaches a failure terminal
  state (s_store_retry flag; enqueue failure falls back to the failure page); BACK = no cancel,
  return to the progress page and keep waiting. Precise terminal-state messages (Cancelled. /
  Checksum mismatch. / Version changed. etc.) land in the job message on screen; esp_err_to_name
  is only a fallback. ③ Configurable timeouts: new `main/Kconfig.projbuild` —
  `CONFIG_META_STORE_SESSION_TIMEOUT_MS` (default 300000, range 30s~24h) and
  `CONFIG_META_STORE_HTTP_TIMEOUT_MS` (default 30000); the session timeout also gets a runtime
  override `meta_store_session_set_timeout_ms()` (same clamp range, for later settings page/NVS
  persistence); both fall back to the same defaults when sdkconfig is absent (host stub build). ④
  Progress confirmation re-verified: the download loop refreshes the percentage each chunk as
  `received*100/content_len`, P4 shows "N%  X/Y KB", and at the end a triple comparison against
  the analyze summary + HTTP headers (implemented since r4; re-verified, no changes).
- **v3.2-r5 (2026-09-25) Code review fixes (k3 review)**: full-plan review; fixes: ① **fatal**:
  `meta_store_net_init()` did not take the slot registry; the network module's `s_slots` stayed
  NULL → install always failed; init signature changed to
  `meta_store_net_init(meta_slot_info_t slots[META_SLOT_COUNT])`, both call sites pass the
  launcher's static registry. ② **contract**: `/api/analyze` business results (including
  supported=false reason codes) are always 200 + JSON — the old 422 would be flattened by the
  device into unavailable, hiding too-large/wrong-chip; id validation tightened to `\d{1,7}`. ③
  **semantics**: install failures split in two — before esp_ota_begin (network/TOCTOU checks) the
  slot registry is untouched (flash untouched; a pure network error must not wipe existing slot
  info); from begin onward (flash possibly erased) invalidation proceeds as before. ④ **UI**: P5
  done page uses a dedicated slot variable (store_goto clears s_sel, which previously always
  showed slot 0); the 5-minute store session timeout auto-extends while an analyze/download job
  is running (slow networks and big images no longer killed mid-flight). ⑤ **robustness**:
  `meta_store_json_get_int` overflow check moved before multiply-add (removing signed-overflow
  UB); stale GOT_IP/DISCONNECT event bits cleared before waiting (leftovers from a previous
  round no longer punch through). ⑥ **readability/de-dup**: removed the dead `s_wifi_mode_ap`
  state, dead `s_store_timer_on` flag, unused `esp_sntp.h` include, unreachable detail copy in
  the loadMerged failure branch; corrected two misleading comments ("auto-retry once on
  disconnect" etc.); the post-SNTP-timeout ONLINE log no longer claims "clock synced"; hex64
  uses a lookup table; missing content-length (chunked) logs no longer claim "length mismatch".
  Verification: node 10 cases, host JSON tests, `-fsyntax-only` stub checks, `check_repo.py`,
  `validate.sh --static` all green.
- **v3.2-r4 (2026-09-24) Implementation complete**: main-branch implementation per this plan.
  Server: `tools/install-slot/store-analyze.js` (pure-ESM analyze/separate core, all deps
  injectable, directly reusable on Cloudflare Pages) + `server.mjs` new `/api/analyze?id=N`,
  `/api/extracted?id=N` (binary stream + `x-image-len`/`x-image-sha256` headers); Node unit
  tests `test-store-analyze.mjs` 10 cases pass; real play 563 smoke test passed (analyze
  returns supported/suggestedSlot=0; extracted stream SHA-256 matches the response header).
  Firmware: `meta_store_json` (bounded JSON extractor, host tests pass), `meta_store_api`
  (analyze + streamed OTA, computing SHA-256 while downloading, double-compared against
  analyze/response headers), `meta_store_net` (provisioning SoftAP form (no pairing code) /
  STA / SNTP / job-queue network task); `main.c` STORE six pages (P0 provisioning → P1 ID → P2
  detail → P3 slot → P4 progress → P5 reboot prompt); `meta_net`/`meta_import` and their tests
  deleted along with the upload channel; custom two-root bundle (GTS Root R4 + ISRG Root X1,
  `main/certs/`) replaces the full Mozilla bundle. Host checks: `test_meta_store_json.c` all
  pass; both ESP-IDF modules pass IDF 5.x signature-stub `-fsyntax-only`;
  `tools/validate.sh --static` all green. Real-device build (`idf.py size` size gate) and
  on-device end-to-end remain acceptance items (§5 P4/P5). Newly discovered during
  implementation: play 563's partition table contains a `recovery` app partition (the firmware's
  own fallback) — the custom-partitions whitelist narrows to data-type partitions (type=1)
  only; app-partition contents are inert in the target layout; the analyze contract's failure
  branches carry no slots array (the device unsupported page shows only the reason).
- **v3.2 (2026-09-24) Scope convergence**: this phase only builds "numeric keypad play-ID entry →
  server-side unpack analysis → slot selection → OTA download into slot"; **login/favorites/
  category browsing removed** (login interaction too costly on the device; browsing stays on the
  official marketplace); P0 restored to a single provisioning step; the server correspondingly
  keeps only the two new analyze/extracted endpoints. Interface research for the removed
  features stays in Appendix A.
- **v3.2-r2 (2026-09-24) Information entry convergence**: the device no longer pulls store
  details locally; app name/installability/smallest slot come only via one `/api/analyze`
  request; analyze response gains a `name` field (server takes the slug, ASCII-safe); all
  device-side slug display/MNAM writes switch to `name`; `reason` gains `not-found`; firmware
  drops the `/api/play` call (the server still uses it internally).
- **v3.2-r3 (2026-09-24) Feasibility check**: ① firmware size — net increment estimated
  +90~170KB (mbedTLS crypto primitives already linked via meta_sign; the bulk is the TLS/X509
  layer and cert bundle; a custom two-root bundle keeps it ~+2KB), planning reserve ~120KB,
  with `idf.py size` as the entry gate (§2.4); ② Cloudflare Pages feasibility — the unpack chain
  is zero-Node-API pure ESM and can run directly in Pages Functions; cache moves to R2 (strongly
  consistent) + KV (rate limiting); free-tier 50ms CPU per request needs load testing, upgrade
  to the $5/mo paid tier if exceeded (§1.3).
- **v3.1 (2026-09-24)**: favorites switched to the official marketplace login plan (SMS/password
  login + Cookie passthrough) — deferred wholesale with v3.2.
- **v3 (2026-09-24)**: architecture inversion — unpacking moves out of firmware to the chuanxilu
  server; single device trust anchor; analyze slot suggestions / unsupported display.
- Conventions inherited from v2 and still valid: threading model, SNTP, retry/cancel matrix, MNAM
  ASCII constraint, battery prompt, emulator WiFi end-to-end, IDF v5.5.3 baseline, etc. (§2.3, §4, §5).

---

## Appendix A: Reserved for Later Iterations (Endpoint Facts Researched and Confirmed, Not Implemented)

**A.1 Official marketplace login and favorites** (research completed in v3.1):
- SMS login: `POST /api/auth/phone-login/verification-code/request` `{phone}` →
  `POST /api/auth/phone-login` `{phone, verification_code, locale}`;
  password login: `POST /api/auth/login` `{email, password}`; session cookie + CSRF
  (`GET /api/session` issues csrfToken); logged-in state `GET /api/me` → `{ok, user|null}`;
- Favorites toggle `GET/POST/DELETE /api/me/favorites/play/<id>` (GET returns `{saved}`; separate
  from "follows" `/api/me/follows/...`); favorites list `GET /api/me/favorites?kind=play`
  (401 when logged out);
- Device interaction sketch: provisioning page step 2 "marketplace login" (phone browser enters
  the phone number/receives the code, device relays the code request), Cookie stored in NVS
  `mpsess`; chuanxilu adds `/api/auth/*`, `/api/me/*` Cookie/CSRF passthrough whitelist;
- Phase 1 items to measure: favorites list response shape, favorites toggle CSRF header name,
  session expiry behavior.

**A.2 Category-list-detail browsing**:
- Data source: the store list response carries `categoryCounts` (key→count) and `discoveryTags`
  (key/name.zh/en/sortOrder); no standalone category endpoint; chuanxilu's `/api/plays` proxy
  currently **drops both fields** — browsing needs them passed through (recommend an extra
  `asciiName` short label, since the device screen has no CJK fonts);
- Known category keys: must-play / multi-device / child-friendly / games / productivity /
  social / learning / information / developer / media.

---

## Appendix B: On-Device Acceptance Script (r10.4) — Expected Screens and Failure-Layer Table

Flash `build/meta-pass_v1.0.0-<n>-*.bin` (v23 or later). Walk the steps in order; on any
failure the screen shows the failing layer — match it against the table, no guessing.

| Step | Action | Expected screen |
|---|---|---|
| 1 | Hold UP, plug USB, web upgrade | Upgrade progress, device reboots into launcher |
| 2 | Enter STORE | P0: hotspot name or `WiFi: <saved>` + `double-UP = change WiFi / hold OK = exit` |
| 3 | (saved credentials) wait | `Connecting to WiFi… (Ns)` → `Syncing clock… (Ns)` → `Online. WiFi: …` → auto to P1 in 2s |
| 4 | Type 563 on keypad, OK | `Fetching info…` then name/size/`min slot: 0` + `NOTE: custom 'easter' part…` |
| 5 | CONFIRM → pick slot 0 | `SLOT 0 OK`, then download %, `Installed.` |
| 6 | OK LONG anywhere | Back to the launcher list (single stable exit) |

Failure-layer table (screen text → layer → action):

| Screen | Layer | Meaning / next step |
|---|---|---|
| `TLS failed (clock unsynced).` | Device: SNTP | Clock not synced → certificate time check cannot pass. Retry once; if persistent, capture serial log (`store_net`) |
| `TLS/DNS failed. Retry.` | Device: TCP/TLS/DNS | DNS or TLS handshake failed while WiFi shows online → retry; persistent = router blocks the CDN |
| `No response. Retry.` | Device: HTTP | Connection up but no headers → server-side stall, retry |
| `Connection lost. Retry.` | Device: HTTP | Body read interrupted → retry |
| `Bad response from server.` | Server: contract | Response not contract-shaped → capture timestamp, check deployment |
| `Server error <code>` | Server: 5xx | Worker/upstream error; code tells which side, retry |
| `unavailable` + `upstream 503 (metadata/image download)` | Server→market | CF worker could not reach the marketplace; transient, RETRY |
| `unavailable` + `play metadata missing firmware fields…` | Marketplace data | Play record incomplete server-side; report the play id |
| `unavailable` + `worker exception: …` / `server exception: …` | Deployment bug | Exception escaped the analyzer; report the detail line verbatim |
| `Not supported: not-found` | Marketplace data | Play does not exist; check the id |
| `Not supported: too-large` | Policy (final) | App larger than every slot; no retry |
| `Last try: Wrong password? / AP not found…` | Provisioning | Credentials wrong or router hidden/PMF; re-provision |

Data-plane evidence gate before any flash: `node tools/e2e-production.mjs` — 15 checks, all
must PASS (analyze contract, extracted len/sha/magic, device-parser re-feed, cert anchor,
reason alignment, unknown-id error contract).
