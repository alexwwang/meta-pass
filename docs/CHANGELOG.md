<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- **Dedicated DATA E2E child firmware baseline for the mobile page**: adds a standalone ESP-IDF 5.5.3 test app with a USB Serial/JTAG command protocol. It writes, reads, and validates a deterministic record through the real `esp_partition_*` API resolved by the `e2edata` label; a host serial client and static contract gate are included. The runbook specifies A/B isolation by distinct play IDs, post-reboot reads, and uninstall checks. It also explicitly records the remaining blocker: a default-off `CONFIG_META_E2E_TEST_CONTROL` launcher channel and a runtime driver matching the existing runner are now included. They have not yet been built in ESP-IDF or validated on hardware, so full automated E2E must not be claimed.

- **Front-end module × real-device regression gate**: the phone-side install
  path and space management were only ever exercised against a mock device
  (`tests/test_phone_install.mjs`), so mock↔firmware drift was invisible — the
  truncated `/api/install/status` body below would have passed the mock and
  only surfaced on hardware. New `tools/realdevice/browser_smoke.mjs` drives
  the real `install-slot/phone-install.js` with real `fetch` against the same
  board: P1 `preflightMeta → prepareImage → geomFromListing → runInstall`
  (2 MB store download, verify, extract, carve proposal, chunked upload,
  finalize, slots readback, remove→archived), P2 the Child DATA branch via a
  synthesised full flash image (`extent` 8192 B > initial payload 2048 B,
  parsed by `extractDataImages`, not hand-assembled), P2.5 both browser-side
  reject gates firing before any upload, P2.6 the record survives an esptool
  soft reset. 20/20 PASS, exit 0. Orchestrated by `run_browser_smoke.py`
  (restores the persisted NVS token, so no physical pairing press).
  Evidence is redacted at the source — token, LAN IP, USB serial and host
  home dir — and the full run output is inlined into
  `docs/storage-test-report-20261008(.zh_CN).md` §6 instead of uploaded:
  `realdevice-smoke.yml` dropped `upload-artifact` for `logs/**` (the raw
  logs carry the LAN IP and host username) in favour of a redacted verdict
  line in the run summary. `validate.sh --static` syntax-gates both harness
  scripts, and the workflow runs the regression right after the protocol
  smoke — still `workflow_dispatch` only, with no board flashed in cloud CI.
- **Real-device smoke (feat/storage handoff): two firmware fixes proven on
  hardware**. S0–S6 now all PASS (evidence: `tools/realdevice/logs/
  20261008-121330/`, report `docs/storage-test-report-20261008.md`).
  (1) `/api/install/status` sent a body missing its final `]}` —
  `h_install_status` counted the snprintf tail in neither `off` nor the send
  length; now uses `httpd_resp_sendstr` like `h_install_session` (S2 crashed
  the harness with `JSONDecodeError: column 283` = 2 bytes short).
  (2) `meta_carve_flash_sync_states` rebuilt slot entries from the runtime
  table with memset and dropped record-side `play_id`; every install's
  backfill commit zeroed it, and `archive_slot_and_data` then died with
  `ESP_ERR_INVALID_STATE`, so uninstall never archived DATA (S4). Backfill
  now preserves `play_id`; regression assertion added to
  `test_meta_carve_flash.c::test_sync_states`. Also fixed the realdevice
  smoke's S6 flash-read length (`{len:x}` rendered 4096 as decimal 1000) and
  documented the broken py3.14 IDF venv workaround (`IDF_PYTHON_ENV_PATH`).
  Physical power loss remains UNTESTED (soft reset only).
- **Test suite: close out stale phone/pool contract assertions left by
  d4209a0** ("survive controlled reboot", v2.0 dev). That commit changed two
  behaviors without updating the host tests, leaving `test_phone_install.mjs`
  red since Oct 4 (first failure masked the rest): (1) bridge GET calls retry
  network errors with backoff, so a single `status()` call now absorbs the
  post-remove reboot window instead of reporting `status: 0` — 9b rewritten to
  assert the transparent-absorption contract plus a beyond-retry-budget
  unreachable branch; (2) `geomFromListing` fit is `empty`-only, so occupied
  slots are no longer selectable/suggested — 10a rewritten to assert the
  occupied-slot rejection, and the `test_dynslot_pool.mjs` withStorage fixture
  switched its app slot to `empty` to keep exercising the existing-slot-first
  branch.

## v2.1.0 (2026-10-06)

- **USB installer: dynamic (dynslot) slot management page**: step 2 of
  `install-slot.html` stops being a static 3-radio list. On Connect the page
  reads the carve record (A/B sectors at 0x35A000) plus the device partition
  table and renders a live slot table: one row per carved slot (offset/size/
  state/name/max image), a mode badge (`dynslot (dynamic slots)` /
  `dynslot (no carve record yet)` / `fixed 3-slot (legacy)` / `factory
  fallback`), a summary line (slot count, used slots/bytes, data bytes, free /
  total, largest gap) and a per-row **Remove** button in dynslot modes. A
  dashed `Auto` row shows the exact planned geometry for a new slot once a
  firmware file is chosen (`new @offset · size`, or `needs X, largest gap Y,
  free Z` when the pool cannot fit it); the recommended row mirrors the
  device's suggested-slot rule (first empty fit, else Auto) and the checked
  radio follows it until the user picks another row. **Install** resolves the
  target through the shared planner: creating a slot commits the record first
  (seq +1, A/B rotation, read-back verified), materializes the 0x8000 table
  (byte-compared), then writes the payload and finally commits VALID +
  image_len + SHA-256 + display name; overwriting an existing slot skips the
  geometry step. **Remove** confirms by name/offset/size, erases the slot's
  first 4 KB (anti-resurrection, bytes beyond the header stay until reused),
  commits the record without the slot + re-materializes the table, and the row
  disappears with free space updated immediately — same order and guarantees
  as the device's `POST /api/install/remove`. All new strings are bilingual
  (EN/zh) and a language switch re-renders the dynamic table live. New pure
  module `install-slot/dynslot-record.js` (byte-exact record codec + planner +
  view model, shared by page and host tests, pinned by C-generated golden
  fixtures) and `install-slot/mock-device.js` + `?mock=1|fresh|legacy` page
  mode with a persistent MOCK banner for hardware-free browser testing
  (local server whitelist updated for the two new modules). Host gate:
  `tools/install-slot/test-dynslot-record.mjs` (12 PASS) registered in
  `tools/validate.sh`; browser walkthrough verified connect → list →
  auto-allocate install → remove → summary with byte-level flash read-back.

- **USB installer: occupied slots are never targeted by default; overwrite
  requires confirmation**: human review of the dynamic slot page found that
  the radio fallback ("first fitting row") could silently aim an install at a
  slot that already has a sub-firmware installed. In dynslot modes the default
  now resolves recommended empty slot -> `Auto` (when the pool fits the image)
  -> nothing, so loading a firmware can never pre-select an occupied slot;
  with no default, Install answers `err_pick_slot_first` instead of writing.
  Installing into an occupied VALID slot pops a `confirm()` naming
  slot/offset/size/name -- cancel logs "Install cancelled -- nothing changed"
  with zero writes. Removing a slot voids an explicit radio pick (row indices
  shift after the splice, so a stale index must not retarget another slot).
  The disabled `Auto` row now states the reason up front ("no room: needs X,
  largest gap Y, free Z -- delete a slot for a contiguous span"). Legacy
  fixed-3-slot devices keep the old pre-checked default. Verified in mock mode
  end-to-end: both slots occupied -> no default pick -> guided error; explicit
  pick -> confirm -> cancel leaves seq/flash untouched, accept overwrites.

## v2.0.0 (2026-10-04)

- **v3.2-r10.38 (2026-10-02) dynslot — dynamic slot partitioning + slot removal
  (feat/dynslot)**: the fixed 3-slot table becomes a pool + carve model — two
  pools (0x180000–0x356000, 0x360000–0x7FE000), up to 8 slots, 128 KB minimum,
  64 KB offset alignment, 4 KB size granularity (`main/meta_carve*` pure core
  with host tests, A/B carve record in `store`, materialized 0x8000 table,
  bootloader-hook authority with safe-table first boot, legacy 3-slot migration
  that relocates the Wi-Fi credential backup from 0x35A000 to 0x35E000).
  Install path: the phone's carve proposal (`carveOffset`/`carveSize`) is
  re-run through the device's first-fit allocator and any divergence is
  rejected; a new slot materializes before upload (prepare returns 503 → the
  phone resends with the persisted token → reboot). **Slot removal (new)**:
  `GET /api/install/slots` lists the live carve (state/name/size/limit/offset/
  kind + free pool bytes) and `POST /api/install/remove {"slot":N}` erases the
  deleted slot's own data, commits the record and re-materializes the table,
  replies 200, reboots after 150 ms — power-loss safe at every step (busy →
  409, bad shape → 400, not in carve → 404; token + Origin gated). No data
  moves: the hole is reclaimed by first-fit on the next install. Web side: an
  "Installed" management panel (list, two-step delete confirm, wait for reboot,
  refresh) and the shared `install-slot/dynslot-pool.js` module (pool
  descriptor + allocator with the same constants and first-fit algorithm as
  `meta_carve.c`, device-derived slot claims with a legacy fallback, 8-slot
  install bound). Worker and local dev server now route `/dynslot-pool.js`
  no-store. Host gates `--static` and `--firmware` PASS; device E2E (HTTP
  removal, power-cut injection) and QEMU hook cases NOT RUN. Worker-side
  analyze limits still derive from legacy `SLOT_GEOMETRY` — full three-way
  unification is tracked in `docs/assets/dynslot-data-unification-research.md`.

## v1.1.0 (2026-10-02)

Store install release: the flagship feature is the market install chain — the phone
web module (search → analyze → download → verify → extract → slot-fit → upload) drives
the device's LAN install service, and the production metapass site (`metapass.
chuanxilu.net`) serves the Worker proxy plus the remotely-loaded phone module. Slot
interaction collapses to the list page: OK boots the highlighted slot in one press
(signed or unsigned; the warning/detail/delete-confirm pages are gone — upstream
48e85590 semantics), and the easter egg lives on the highlighted slot. Two upstream
fixes ported: child-firmware deep-sleep wake (GPIO holds + otadata renewal) and the
QR/wifi reliability rounds. The four contracts frozen at v1.0.0 (hybrid single-file
MPUPV2, backup manifest v1, signature format, 3-slot partition layout) are unchanged;
the device↔phone protocol is backward compatible (`manifest.slot` is optional — old
firmware ignores it and keeps the physical-confirmation flow). CI deploys through the
same `tools/pages-deploy.py` REST path as local production deploys. Firmware artifact:
`build/meta-pass_v1.1.0.bin`.

- **v3.2-r10.37 (2026-10-02) CI unification + glibc fix**: `workers.yml` now deploys
  through the same `tools/pages-deploy.py` REST path used for every production deploy
  (wrangler-action dropped — wrangler 4.145.0 `pages deploy` hangs after upload,
  verified locally); the script accepts `CF_API_TOKEN` for CI while the local OAuth +
  refresh-token rotation path is unchanged. First main push surfaced two CI-only
  failures: bun is not preinstalled on ubuntu-latest (setup-bun step added, SHA-pinned),
  and `strtok_r` in `meta_store_prov.c` needs `_POSIX_C_SOURCE` under `-std=c11` on
  glibc (macOS is lenient, so local `--static` stayed green).

- **v3.2-r10.35 (2026-10-02) one-press boot — detail and delete-confirm pages removed
  (user final)**: the slot list is now the only slot-interaction page. OK boots the
  highlighted slot directly (signed or unsigned, zero confirmation; unbootable slots
  silently no-op); the easter egg is the sole hidden flow — UP UP DOWN DOWN rapid presses
  (PRESS-qualified, zero interference with CLICK row navigation) on the highlighted slot.
  Device-side delete is gone (web re-flash covers it). Aligns with upstream 48e85590
  semantics plus this branch's store flow.

- **v3.2-r10.34 (2026-10-02) upstream ports — deep-sleep wake + unsigned quick-boot**:
  from `jiandanc/meta-pass`. (1) Child-firmware deep-sleep wake had two independent
  defects: the child's GPIO holds survive reset and swallow all launcher SPI commands
  (backlight on, black screen — fixed by releasing global + per-pin holds before SPI
  takeover, then 0x11 SLPOUT + 120 ms before panel reset, abandoning the SWRESET-first
  order); and wake-from-deep-sleep is a full boot, so the child's otadata copy still
  read PENDING_VERIFY and the rollback logic marked it ABORTED, dropping to factory
  (fixed by branching on reset reason in the bootloader hook — deep-sleep wake renews
  the running child's copy to VALID after CRC re-check, erase-before-write; cold reset
  keeps the original rollback policy). Upstream's 6-case wake-contract test +
  `must_resume` full-state coverage imported into `--static`. (2) Unsigned firmware
  boot no longer shows a warning page (48e85590 semantics, user-trimmed this round to
  the confirm page only; the full page set removal landed in r10.35).

- **v3.2-r10.33 (2026-10-01) R2 firmware materialization dropped (user decision)**:
  `/api/firmware` is a pure streaming CORS proxy again — no server-side cache,
  materialization, or strip. Rationale: phone-side strip costs ~2 ms + double hash
  0.3–1 s at the measured 45 MB/s (<5% of total install time), and the store SHA-256
  gate must run phone-side anyway (trust chain); caching only saved the origin
  download leg while adding cache complexity. The 6 orphan `firmware/*` R2 objects
  were deleted one by one.

- **v3.2-r10.31/r10.32 (2026-10-01) device UI polish**: DONE page shows the actual
  installed `st.slot` (was hardcoded slot0); SCAN ME panel trued to 104 px — 4 content
  rows + status line, title/QR/panel at equal 5 px gaps (y=171); BACK TO LIST
  double-fire fix — DONE judged by CLICK/LONG (not event type, since PRESS+CLICK each
  fired a migration) plus a 400 ms swallow window (`s_list_arm_at`); SCAN ME page
  renews its idle deadline (zero requests while the phone browses; a button touch
  would otherwise air-drop the session).

- **v3.2-r10.30 (2026-10-01) phone install UX overhaul — interaction moves to the
  phone (user decision)**: `manifest.slot` becomes an optional protocol field (≥0 =
  phone-selected slot: prepare arrives pre-confirmed, skipping the device's P2/P3
  physical confirmation; absent/-1 = the legacy device-physical flow, so old firmware
  stays compatible). Phone side: a slot selector with oversized slots disabled
  ("needs x.x MB > limit x.x MB"); full UI redesign in the impeccable Operate mode with the
  device pixel palette (ink/paper/sky/grass); the panel becomes an overlay (bottom
  drawer on mobile, centered on desktop, mask-tap closes); category first-level menu
  (All + per-category counts) with tag chips = discoveryTags via `tag=` filtering
  (verified live: `tag=multiplayer` works; three pseudo-category keys blacklisted for
  upstream 502s); offset+limit pagination per the official contract (`page/pageSize`
  silently ignored; `limit≥100` → 502) with IntersectionObserver auto-load; install
  rename with composition awareness (IME double-input guard, ≤32 printable ASCII);
  `displayNameFor` chain (MNAM real name > title.en > community- prefix-stripped slug >
  Chinese title > `play <id>`); every failure (unsupported / oversized / download
  pipeline / runInstall) lands in the failSheet overlay; hero gets a three-line guide.
  Critical fix: `extractAppImage`'s default cap was hardcoded to the 2 MB slot
  (SLOT_CAPACITY 2093056) — the root cause of play 792 (2245952 B extracted)
  passing the fit check for slot2 then failing install with "exceeds max 2093056";
  now passes explicitly `max(partSize) - TAIL_SECTOR` = 2740224 (live-verified, PASS
  8g pins it). Coffee QR (`author-coffee.jpg`) in a fixed top-right dock with i18n
  caption. The Worker gains `/api/play?id=`, `/api/analyze` (CORS), and streams
  `/api/firmware` via `new Response(upstream.body)` with a `/api/download/` prefix
  whitelist.

- **v3.2-r10.24–r10.25 (2026-10-01) device reliability — QR and WiFi**: the SCAN ME
  QR rendered blank because `lv_qrcode_update` silently fails on the I1 canvas (and
  `lv_canvas_get_px` reads all-zero on I1, so the probe was invalid) — the lv_qrcode
  widget is replaced by qrcodegen fixed-version-5 direct encoding drawn pixel-by-pixel
  via `lv_canvas_set_px` on an RGB565 canvas (~37 KB static bss: `s_qr_tmp/s_qr_data/
  s_qr_canvas_buf`). WiFi: ONLINE-state `EV_DISCONNECT` now auto-reconnects (ssid/pass
  snapshot, 3-attempt cap, stale bits cleared to avoid false triggers). The "store
  re-entry credential loss" mystery solved — credentials were never lost: exiting the
  store left `ap_start()`'s AP running and `begin()` saw AP_UP and returned
  idempotently; fixed by setting `s_ap_after_teardown` only in `reset_wifi` and having
  `begin()` tear down the AP when credentials exist. Credentials are now dual-written:
  NVS plus a raw-flash backup at the 0x35A000 gap (magic + CRC).

- **v3.2-r10.23 (2026-10-01) mota implementation audit + fixes**: full audit of the
  `feat/mota` LAN-install implementation against the design doc (report:
  `docs/assets/mota-implementation-audit.md`, bilingual) — five blocking and nine
  non-blocking findings, all fixed in-round. Flow-breaking: the boot UI uploaded the
  merged image instead of the extracted app image (`runInstall` length guard always
  tripped), `getPlayDetail` called an unrouted `/api/plays/id/<id>` path (live 404 vs
  200 on `/api/play?id=`), and the Worker `/api/plays` proxy dropped the query string
  (search returned the unfiltered 673-play catalog; live-verified). Device-side: the
  UI-task cancel path raced in-flight chunk writes (`esp_ota_abort` frees a live OTA
  handle — heap UAF, verified against IDF 5.5.3 `esp_ota_ops.c`); fixed with a session
  mutex serializing UI-task entry points with the httpd upload path, plus
  `flash_touched` set before `esp_ota_begin` (the erase window left destroyed slots
  unmarked). Also: prepare re-checks `confirmed` after the body read (a late prepare
  could erase a completed physical confirmation); MNAM name-write failure now fails
  finalize; upload activity (chunk arrivals) instead of the latched `uploading` state
  renews the store deadline, with a 30 s stall threshold; runInstall treats a missing
  `done` as failure and accepts resume at exactly `imageLen`; the Worker proxy streams
  `/api/firmware` (no full buffering) and serves all phone-module imports no-store;
  detail view shows `updatedAt`. Test blind spots closed: the metapass mock now mirrors
  the real Worker route table and executes `q` filtering; new regression tests (boot
  payload, resume-at-imageLen, done-poll timeout); `worker_contract.mjs` gains gates
  for query forwarding, streaming, no-store modules, and phone-path↔Worker-route
  matching. Toolchain note: the local ESP-IDF env was fixed without installing
  unused Xtensa toolchains (`IDF_SKIP_TOOLS_CHECK=1`, see the `idf()` shell
  helper); firmware rebuilt and verified — `meta-pass_v1.0.0-58-gcf0fa8f.bin`.

- **v3.2-r10.22b (2026-10-01) Phone web module MVP — the metapass side of the LAN install**:
  `install-slot/phone-install.js` is the first remotely-loaded module served by the
  Worker (`GET /phone-install.js`, `access-control-allow-origin: *`, no-store; the
  device boot page now points at this URL). Single-file module with a built-in pure-JS
  SHA-256 (`crypto.subtle` is unavailable on the device's plain-HTTP origin) whose 9
  vectors are cross-checked against node:crypto. Flow per design doc §5/§6: market
  search (`q` forwarded through `/api/plays`), detail metadata normalized with the
  fallback chain and the no-firmware-fields = uninstallable gate; integrated preflight
  (analyze → merged-image download via the whitelisted `/api/firmware` → store SHA-256
  verify → local factory extraction → local hash → exact match against analyze → slot
  fit table from the shared `SLOT_GEOMETRY`); then the device session driver —
  prepare, poll for the physical confirmation (slot taken from the device, never the
  phone), session open, sequential chunks with in-place retry (3 no-progress attempts)
  and resume from the device-reported offset after a half-written chunk, finalize,
  terminal-state poll. Refusals: no token, protocol mismatch, busy device, confirm
  timeout (never opens an upload session). Pinned by `tests/test_phone_install.mjs`
  (18 checks against a mock device that mirrors `meta_store_install.c`'s exact HTTP
  contract); registered in `validate.sh --static`.

- **v3.2-r10.22 (2026-10-01) LAN phone-assisted install — device-side cutover (`feat/mota`)**:
  the store-download channel moves off the device. The launcher now shows a QR page
  (120px `lv_qrcode` + IP + 6-digit pair code, text fallback) served with a token; the
  phone web module scans it, runs market search/analyze/download/extraction, and uploads
  the app image to the device over LAN HTTP. Device side: `meta_store_install.{c,h}` —
  a port-80 install HTTP service (prepare/session/chunk/finalize/status/cancel/pair +
  boot page, mutually exclusive with the provisioning portal) with 128-bit one-shot
  upload tokens, mandatory `X-Meta-Session`, same-origin Origin checks, streaming 4KB
  chunk writes with SHA-256 + `esp_image_verify`, upload gated behind physical slot
  confirmation, and INVALID slot marking on failure; `meta_install_model.{c,h}` — the
  pure-logic manifest/session/chunk/finalize rules, host-tested by the new
  `tests/test_meta_install_model.c`. Removed: the numeric play-ID keypad page, ID edit
  model, analyze parser/client and info page (`meta_store_{api,api_fail,analysis,
  info_page,idedit,range}`), WAN TLS/SNTP/certificate-bundle dependencies
  (`esp_http_client`, `esp-tls`, `main/certs/`, SNTP config), and their gates/tools
  (`test_http_contract.py`, `test_download_{speed_config,retry_gate}.py`,
  `e2e-production.mjs`, `verify-crt-bundle-match.py`, `e2e-feed-fixture.c`).
  `sdkconfig.defaults` enables `LV_USE_CANVAS`/`LV_USE_QRCODE` (qrcode builds on the
  canvas class); the lwIP 32KB window stays — LAN upload benefits the same way WAN
  download did. The phone module (`install-slot/`) and Worker (`_worker.js`) are
  unchanged in this cutover; `tests/worker_contract.mjs` still pins the Worker API
  surface.

- **v3.2-r10.21 (2026-09-30) Fast zombie-connection kill during OTA**: the first on-device
  run of the r10.17-r10.19 stack (v50, play 675) proved resume end-to-end —
  `install resume 2/6 at 900188/2664256`, 2.4s TLS reconnect, no full restart, image
  verified — but the dead first connection burned ~150s on the 3x30s EAGAIN ladder plus
  20-29s slow reads before being declared dead (t=225s to t=373s moved only 36KB). While a
  connection is a zombie (TCP open, no data), waiting is strictly worse than reconnecting:
  resume costs only a ~2.5s handshake and keeps every byte already written. The install
  per-read timeout therefore drops 30s to 15s (`DL_READ_TIMEOUT_MS`), consecutive-EAGAIN
  tolerance 3 to 2 (worst-case zombie detection 30s vs ~150s), and the connection budget
  rises 6 to 8 to spend the savings on reconnects. analyze keeps the 30s timeout (single
  small response, no resume semantics). Pinned by `test_download_retry_gate.py`. Context
  from the same run: the link burst to 67kB/s while averaging 5.7kB/s — stalls are
  path/WiFi-side, not server-side (R2 serves host curl at ~630kB/s); the firmware cannot
  fix the link, it can only keep making progress, which is exactly what fast-kill + resume
  does.

- **v3.2-r10.20 (2026-09-30) Audit H1-H8 closed**: all nine handover items from the
  r10.17-r10.19 code audit are addressed (H9 = on-device evidence, still open by design).
  H1 rate limiting is now real: a `RATE_KV` KV namespace is bound in `wrangler.toml`
  (the audit found the counter existed in code but was never bound, so production never
  returned 429); verified live — out-of-range probes yield `416,416,416,429,429,429,429`.
  H2 the edge-cache key carries only the play id (ts/sig stripped), so the analyze prewarm
  now serves ticketed firmware requests instead of splitting into one-shot 2.6 MB entries;
  verified live — `analyze?id=100` prewarm, then a different-query extracted request, served
  `x-source: edge` twice (the `x-source` response header was added to edge entries for
  exactly this observability). Note: the Workers Cache API is a no-op on `*.pages.dev`
  preview hosts (no zone); cache hits only happen on the production custom domain.
  H3 ranged responses set `content-length: total - rangeStart` explicitly instead of relying
  on runtime normalization of `obj.size`; verified at first/mid/last offsets
  (`CL == body == total-off`). H4 cold-path persistence (edge cache, R2 object, `.meta.json`)
  moved into `ctx.waitUntil` with independent buffer copies — the device gets first byte
  before any 2.6 MB write; `x-r2-write` became a background-only log line. H5 Range resume
  requests honor tickets for 3600 s (the 600 s analyze window cannot cover slow-link resumes;
  non-Range requests keep 600 s; `?range=1` on r2check diagnoses the long window).
  H6 edge-cache hits are re-checked against the current analyze sha/imageLen before serving
  and stale entries are evicted — the firmware treats a mismatched digest header as fatal.
  H7 a fully received image exits the retry loop straight to verification (a connection
  dying after the last byte no longer triggers a wasted `Range: bytes=<image_len>-` attempt
  and full re-download); pinned by `test_download_retry_gate.py`. H8 dl.sig/dl.ts parsing has
  host coverage for five forms (valid / missing / ts=0 / non-hex / wrong-length), and the
  parser now validates 16 lowercase-hex chars per character so mangled tickets never reach
  the install URL; pinned by `test_store_analyze_contract.c`. Gates: `worker_contract.mjs`
  PASS 12/13/14 pin key normalization+freshness re-check, backgrounded persistence, and
  RATE_KV wiring; `e2e-production.mjs` E2E-9 adds live H1/H3 acceptance.
- **v3.2-r10.19 (2026-09-30) Worker resume (206) activates firmware r10.17**: the worker now
  answers firmware resume requests (`Range: bytes=<start>-`) with `206` +
  `Content-Range: bytes <start>-<total-1>/<total>`, unlocking the OTA resume path that r10.17
  shipped dormant (the firmware safely fell back to full-file retries whenever a resume got a
  200). Only the suffix-range form the firmware emits is accepted; every other Range form is
  ignored per RFC 9110 (200 full), and an out-of-range start gets 416 (the firmware discards
  the OTA session and restarts whole). Range requests bypass the edge-cache read and never
  write it — the cache key carries no Range and always stores the full 200 body, so a 206 must
  never land there. Two supply paths: ticketed requests use R2's native range read
  (`x-source: r2-range`, no full-object materialization inside the isolate), everything else
  slices the analyzed `Uint8Array` (`x-source: computed-range`) — the ticket remains an
  R2-only key, so a mid-download ticket expiry still cannot break a resume (no-rejection
  semantics unchanged). The 206 carries the full-image `x-image-sha256` (after resuming, the
  device verifies the complete image) and returns before any cache/R2 write path.
  `worker_contract.mjs` PASS 11 executes `parseSuffixRange` (accept exactly `bytes=<n>-`,
  reject other forms) and pins the firmware-exact Content-Range template, exactly two 206
  paths, 416 on both, and 206-before-writes ordering. Verified live on play 675: 206 +
  `x-source: r2-range`, slice bytes identical to the full body at the same offset, full-image
  SHA-256 equals the analyze-declared value, no-ticket resume degrades without rejection,
  malformed Range → 200, start≥total → 416.
- **v3.2-r10.18 (2026-09-30) R2 materialization behind HMAC download tickets**: the extracted
  artifact is now materialized into R2 (`extracted/<id>/<storeFwSha256>.bin` + `.meta.json`
  audit mapping) and served straight from R2 — `x-source: r2` — which removes the per-isolate
  cold-render origin hop (the remaining source of multi-second TTFB stalls). Tickets are
  R2-only keys, never an admission gate (user-set rule): analyze issues
  `sig = HMAC-SHA256(DL_SECRET, id:ts)` truncated to 16 hex chars; a valid ticket unlocks the
  R2 read/write paths, while missing/invalid/expired tickets simply fall back to the legacy
  edge-cache path — old firmware without `sig` keeps working and nothing is ever rejected
  with 403. The device parses optional `dl.sig`/`dl.ts` from analyze and appends them to the
  install URL. Rate limiting (per IP+id) applies to both paths before ticket checking.
  Two production bugs were caught by live verification and are now pinned by gates:
  the truncation was applied after hex-encoding (and once at the wrong width), which made
  every issued ticket 32 chars and therefore always invalid (`x-r2-write: skipped` in
  production) — `worker_contract.mjs` PASS 10 executes `hmacHex16` and asserts a 16-char
  output, PASS 9 pins the R2-gating/no-rejection semantics. Verified live end-to-end:
  ticketed first request materializes (`x-r2-write: ok`), second request serves
  `x-source: r2`, and every path's bytes hash to the analyze-declared SHA-256
  (`40de1562…`). Deployment note: local wrangler 4.80/4.143 under Bun's `node` shim died
  silently after the first API request; deploys require real Node
  (`PATH=/usr/local/bin:$PATH`). `DL_SECRET` is a Pages production secret, never in the repo.
- **v3.2-r10.17 (2026-09-29) OTA resume + response-header trust fix**: firmware downloads no longer restart from byte 0 on a recoverable read failure. OTA/SHA-256 state survives TLS reconnects; the next connection requests `Range: bytes=<received>-`, requires `206` plus an exact `Content-Range`, and only aborts the OTA session after final failure/cancel or verification failure. Six segment connections (1s..5s backoff) replace the old three whole-image retries. A legacy server that answers a resume with `200` is never appended at the old offset: the firmware safely falls back to the previous full-file retry behavior. The update also fixes a silent trust-chain no-op — IDF 5.5.3 `esp_http_client_get_header()` reads *request* headers, so the old `x-image-sha256` response-header comparison never ran; response headers are now captured through `HTTP_EVENT_ON_HEADER`. New zero-IDF `meta_store_range.[ch]` host tests and the rewritten `test_download_retry_gate.py` pin EAGAIN handling, 200/206 discrimination, legacy fallback, OTA-state lifetime, and the request-vs-response header trap. `sdkconfig.defaults` now pins the r10.16 WiFi dynamic RX buffer count (48), and gates reject local `sdkconfig` drift for RX buffers and `CONFIG_MBEDTLS_SSL_RENEGOTIATION=n`. The CF worker is intentionally unchanged while its R2 redesign is in progress; resume activates automatically once the server emits 206/Content-Range.
  OTA begin/write failures now display `Flash operation failed.` instead of a generic download error.

- **Store downloads served from the edge instead of re-rendering per request (r10.15, server side)**: the v44 device log showed installs limping at ~4 kB/s with 502s and 30 s TTFB pauses while the same minute curl fetched the same artifact in seconds — the Pages worker was re-fetching the 3 MB merged image from the folotoy.cn origin (and re-verifying/unpacking it) on every cache-cold request, and that origin hop is itself the throttled link. The sample installer project's fast path is exactly this lesson: its device downloads a static firmware URL, never a per-request render. The worker now caches both origin fetches in the CF Cache API (merged image by URL for 1 h, play metadata for 60 s keeping the revisionId recheck window bounded) via the shared `cachedOriginFetch` layer — analyze (P2 detail page) and extracted (download) share it, so browsing a play pre-warms the edge and the install then streams from cache. The trust chain is untouched: every cold build still verifies the store-published SHA-256 before unpacking, so cached bytes can never bypass validation. Gate: `worker_contract.mjs` PASS 8 pins the cache layer, both TTLs, and the unconditional trust chain. Deploy note: CI deploys the worker from `main` only — this lands on the device path after merge (or manual `wrangler pages deploy`).
- **Install survives server-side stalls instead of dying at the first 30 s pause (r10.13)**:
  the v43 device log proved the throughput config live (571 kB/s at 0%) but the download died
  at 45 KB/2.6 MB with 10-30 s per-read pauses and `errno=11` — while the same minute the same
  endpoint served curl with the device's UA in 3.3 s / 16.3 s / 4.4 s at 0% ping loss. The store
  serves this deterministic artifact with `cf-cache-status: DYNAMIC` + `no-store`, so Cloudflare
  re-renders from origin per request and the origin intermittently stalls; Range resume is
  unsupported (returns 200 full). IDF returns `-ESP_ERR_HTTP_EAGAIN` on a read timeout with the
  connection still alive — the code treated every negative read as fatal. Now: EAGAIN continues
  (bounded, 3 consecutive = dead), other read errors or EOF truncation abort the OTA and retry
  the whole install on a fresh TLS connection (3 attempts, 1 s/2 s backoff, cancel honored);
  length/hash contract violations remain deterministic and are surfaced as before. Gate:
  `tests/test_download_retry_gate.py`.
- **Store download throughput: three stacked defaults removed (r10.12)**: the download's
  steady-state ~tens-of-KB/s had three independent caps, each now lifted. WiFi modem sleep
  was never disabled (IDF default `WIFI_PS_MIN_MODEM` dozes the radio between DTIM beacons) —
  `esp_wifi_set_ps(WIFI_PS_NONE)` on both the STA and APSTA start paths. The lwIP TCP receive
  window was 5760 B (4*MSS default), capping throughput at window/RTT ≈ 57 KB/s at ~100 ms RTT
  to the store host — now 65535 (the no-scaling ceiling; `LWIP_WND_SCALE` needs PSRAM, absent
  on C3), with send buffer matched and the tcpip RX mailbox 32→64. The download chunk was 1 KB,
  paying read+sha256+`esp_ota_write` call overhead per KB — now 4096 (flash page, the
  device-proven sample installer's value). CPU 160 MHz, flash 80 MHz DIO, WiFi AMPDU TX/RX and
  hardware AES were already in place. New gate `tests/test_download_speed_config.py` pins all
  of it; per-5% serial telemetry from r10.10 gives the before/after comparison on device.
- **Boot-time slot scan stopped logging bootloader-format errors for half-written slots (r10.11, BUG-21)**: starting the USB monitor resets the chip (USB-Serial-JTAG hard reset, the monitor's default — use `idf.py monitor -- --no-reset` or `ESP_IDF_MONITOR_NO_RESET=1` to keep it running), and the fresh boot's slot scan then verified the half-written slot left by a failed download with `esp_image_verify(ESP_IMAGE_VERIFY, …)` — the non-silent mode in which IDF's segment walk logs `invalid segment length 0xffffffff` (ESP_LOGE) when it reads an erased-state 0xFFFFFFFF segment header. The rejection itself was always correct: the slot scanned INVALID and stayed installable-over. The scan now verifies in SILENT mode and logs one WARN naming cause and remedy. Regression coverage: `tests/test_meta_store_scan.c` (host fixture replaying IDF's segment walk; empty/INVALID/VALID states + erase recovery) and `tests/test_bug21_scan_silent.py` (static gate on the silent call, verified against IDF sources); `tests/test_http_contract.py` is now wired into `tools/validate.sh` (it had only been run manually since r10.8).
- **Store UX pass + download telemetry (r10.10)**: the install page's action row now
  reads CONTINUE (was CONFIRM; the row advances to slot picking); the slot-picker info
  window is double-height with muted background so it no longer reads as a second row
  of buttons, and shows the play name + KB; unfit slots render as one short greyed
  "SLOT n too small" row (the old "TOO SMALL (will erase)" overflowed and clipped into
  what looked like garbage). Download loop now logs per-5% throughput and, on read
  failure, the received/expected bytes and how long the last read blocked — a device
  stall at ~13% will name its position and timing on serial instead of a bare
  "Download failed". Host throughput baseline measured 0.8-1.3 MB/s end-to-end, so the
  fast-then-slow shape is device-side radio/link, not server throttling; firmware
  writes each 1 KB chunk straight to flash (no caching layer exists).
- **Warning page's CONFIRM row restored (r10.9, BUG-20)**: on the first device run
  that ever reached a supported analyze result (BUG-18/19 had blocked every handshake
  before P2), the custom-partitions warning page rendered a single BACK row — install
  impossible despite the designed "warning + confirm" flow. The r10.4 renderer had
  dropped the supported branch while the OK router kept it (row even labeled BACK but
  acted CONFIRM). The three page forms are now one pure decision module
  (`meta_store_info_page`) shared by renderer and router, pinned by
  `tests/test_meta_store_info_page.c`; re-fill clears the stale second row.
- **HTTP status code misread from the wrong API fixed (r10.8, BUG-19)**: a cross-check
  against the sample recovery installer (ai-passport-miniapp-installer, device-proven)
  exposed that `esp_http_client_fetch_headers()` returns the Content-Length, not the
  status code — our analyze/install compared its return value against 200, so on a real
  device every analyze (477 B) died as "HTTP 477" and every install (1.9 MB) at the
  length check, even with TLS fully repaired. Status now comes from
  `esp_http_client_get_status_code()` in both endpoints; `received == content_len` is
  enforced after EOF; device User-Agent is a single-source contract replayed by E2E;
  auto-redirects are disabled (a 301 can no longer re-anchor the length contract);
  receive/header buffer 1024→4096; TLS renegotiation explicitly off; network job stack
  8192→10240 (sample-proven value). New gate `tests/test_http_contract.py` pins the IDF
  contract, the stub signature, and every hardening field; the host stub now declares
  the real `int64_t` contract.
- **Chain-tail issuer missing from bundle fixed (r10.7.2, BUG-18)**: v35 added the GTS
  WE1 intermediate yet the device kept failing with the identical
  `No matching trusted root certificate found` / `-0x3000` handshake log. A new static
  reproduction (`tools/verify-crt-bundle-match.py`) replays `esp_crt_verify_callback`'s
  per-depth issuer lookup byte-for-byte over the live chain with a raw ASN.1 parser and
  pins the real cause: the server presents a 3-cert chain ending in the cross-signed
  GTS Root R4, whose issuer (GlobalSign Root CA) is not in the bundle — mbedTLS walks
  every depth, not just the leaf. Fix: `main/certs/globalsign-root-ca.pem` (subject
  byte-equal to the chain tail's `issuer_raw`; `openssl verify` closes the full chain).
  New E2E-8d runs the byte-level device-lookup simulation plus a stale-image guard
  (newest `build/meta-pass_v*.bin` must embed the current bundle verbatim) on every run;
  E2E count 17→19. All three depths now resolve: chain would VALIDATE on device.
- **Device round r10.7 (v1.0.0-33)**: RETRY no longer kicks the user back to an emptied
  id page (BUG-15: busy/offline retry shows a hint and stays; the keypad is preset with
  the last confirmed play id via host-tested `mpd_idedit_set_digits`); the frozen
  "timeout in 300s" on P0 is replaced by the real state message (BUG-16); OPEN transport
  failures now name their cause on screen — DNS failed / Connect timeout / Connection
  refused / Cert check failed (BUG-17, event-handler capture + `meta_store_api_fail_open_text`).
- **List-page corruption after one store visit fixed (r10.6, BUG-14)**: `s_keys` — the P1
  keypad panel array — was still declared `[10]` from the r8 10-key keypad while the r10
  keypad has 15 keys, so every P1 build (which runs automatically 2s after ONLINE) overflowed
  5 pointers straight into `.bss.s_rows` and the head of `.bss.s_slots` (proven from the
  linker map), leaving the slot list page unable to highlight STORE DOWNLOAD with dead keys.
  The size is now single-sourced from `MPD_KEY_COUNT`, and  the main component builds with
  `-Werror` — the two `iteration 10 invokes undefined behavior` warnings that sat in the
  build log since r10 can never be ignored again. Clean rebuild: v1.0.0-31, both warnings
  gone, validate 112 checks + firmware gates green. Process lessons consolidated to ten
  rules (r9–r10.6) in `docs/BUGS.md`; seven of them promoted to enforceable baseline
  rules in `AGENTS.md`.
- **Analyze transport-failure classification (r10.2, v1.0.0-23)**: the long-standing
  "every play shows unavailable on device" class of failures is now diagnosed on screen —
  analyze transport failures classify by stage x clock state via the new pure-logic
  `meta_store_api_fail` module (host-tested): `TLS failed (clock unsynced).` (the real-device
  killer: SNTP unsynced → mbedTLS certificate time validation fails), `TLS/DNS failed.`,
  `No response.`, `Connection lost.`, `Bad response from server.`, `Server error <code>`;
  business reason codes pass through; P2 shows the text verbatim with a RETRY row. Also: the
  over-limit path no longer leaves a stale reason; P1 hint text back to one line; P0
  change-WiFi gesture is a double-press of UP within 600ms (mis-press safe, host-tested
  `meta_prov_upclick`).
- **Store page-flow fix + P1 keypad polish (r10, v1.0.0-19..21)**: ① page-flow made
  explicit and loop-free — OK LONG exits the store to the list page from every store page,
  P0 ONLINE auto-advance is cancelled by any keypress (previously P1's OK LONG → P0 and
  P0 auto-advanced back → inescapable loop with a 2s real exit window); P0 gains a visible
  `> CHANGE WIFI (OK=confirm)` row (UP/DOWN toggles). ② P1 UP/DOWN short presses move the
  key selection around a ring (r9 had mapped them to cursor moves — an invisible caret —
  leaving short-press navigation dead); on-screen ◀▶ move the insert cursor; pinned by
  `test_ring_navigation`. ③ P1 geometry fixed (ID panel 100→84 so the keypad no longer
  overlaps it; OK key height 58→64 so its bottom aligns with the 0 key) and locked with
  `_Static_assert`. ④ Slot wording: `(invalid)` → `(no firmware)` via a single source of
  truth (`meta_slot_list_word`/`meta_slot_detail_word`, host-tested) — EMPTY = erased,
  NO FIRMWARE = data present but not bootable (ota_2 littlefs recordings); both install
  over cleanly (esp_ota_begin erases first); detail page says `Install overwrites it.`
  instead of `Delete it and re-install.`
- **Store provisioning hardening + policy fix (r9, v1.0.0-14..18)**: six real-device
  failure chains root-caused and fixed, plus a production E2E evidence harness. ①
  **Provisioning scan/connect deadlock**: `/api/scan` used to split "start scan" (network
  task) and "read results" (HTTP handler), and the handler returned without calling
  `esp_wifi_scan_get_ap_records` whenever the result set was empty — in ESP-IDF an
  un-drained finished scan leaves the driver in a residue state that blocks both the next
  scan and `esp_wifi_connect()`, freezing the scan list and stalling credential connects
  for their full 30s deadline. Scanning is now handler-synchronous (start → bounded wait →
  unconditional drain), `pmf_cfg.capable=true` (WPA2/WPA3 mixed routers), and disconnect
  reasons surface on screen (`Wrong password? / AP not found. Re-scan. / Auth failed:
  password/PMF? / Handshake timeout.`, new `meta_store_wifi_fail_text`). ② **A
  self-inflicted regression removed**: a per-second "fallback" `esp_wifi_connect()` added
  in the same cycle prevented association — IDF treats re-entry while connecting as
  disconnect+reconnect and each call restarts connect's internal channel scan; event bits
  are sticky so the retry was pure harm. Found by the deferred static-analysis pass
  (F1–F4), which also raised the job stack 6144→8192 (mbedTLS peak), unified WiFi
  teardown/start into the job task, and restricted the P0 RESET preselection to ERROR. ③
  **Frozen panel root cause**: the 300s session overlay armed during provisioning (there
  `busy=false`) and `store_tick` returned early — the panel froze at "Syncing clock..."
  while the timeout label kept counting; the overlay is now suppressed during
  AP_UP/CONNECTING/ERROR, the panel refreshes live with elapsed seconds, and SNTP uses
  `ntp.aliyun.com` primary (5s cap). ④ **ONLINE auto-advance re-anchored**:
  `s_net_online_at` was captured at page build (0 when built during CONNECTING) so the 2s
  advance never fired and ONLINE had no refresh branch; the tick now detects the ONLINE
  transition itself. The provisioning web page no longer claims "saved!" (it says
  "Received…" and points at the device screen). ⑤ **Custom-partition policy corrected**:
  market plays started shipping extra data partitions (`easter` 0x82, voicefs-type 0x81)
  and the r8 hard-reject rule locked out half the marketplace (caught by the production
  baseline check within a day); all non-whitelisted *data* partitions now warn-and-allow
  with `detail=<label>` (only the extracted factory app is installed — partition payloads
  never enter the device), pinned by analyzer PASS 5/5b/5c and the firmware contract test.
  ⑥ **P1 keypad rebuilt TDD**: editing extracted into pure-logic
  `main/meta_store_idedit.{c,h}` (host-tested, same code on device), 4×4 layout — 1-9/0
  digits, DEL/CLR column, `◀ 0 ▶`, OK spanning two rows — insert-cursor editing with
  backspace, long-press = row wrap, explicit commit; the tests caught a digit-mapping bug
  the first layout pass introduced. **Evidence harness**: `tools/e2e-production.mjs` runs
  14 checks against the production site (analyze contract, extracted size/sha/magic
  tri-check, real response bytes fed to the device's C parser, leaf-cert issuer vs the
  device's GTS Root R4 anchor, server↔firmware reason-code alignment) — the same checks
  that caught the policy regression; `tests/worker_contract.mjs` pins the Pages worker API
  surface. Factory budget ~1,147,000/1,507,328 B (~24% free); artifacts
  `meta-pass_v1.0.0-18-g0fd6194.bin` and later.
- **Store UX + custom-partition policy (r8)**: four real-device fixes. ① **Scan on
  demand**: the r7 3s periodic background scan kept switching the radio channel
  while the SoftAP was up, so the provisioning beacon had gaps and phones could
  not see the hotspot at all; `/api/scan` now triggers exactly one scan and waits
  up to 2.5s for it (`PROV_SCAN_WAIT_MS`), leaving the AP beacon untouched the
  rest of the time. ② **P1 ID entry rebuilt as an on-screen keypad** with
  variable-length IDs (1–7 digits, matching the server's `\d{1,7}`): 0–9 keys in
  two rows plus a tall GO key and a CLR bar, UP/DOWN move the selection, OK
  appends, the 7th digit auto-commits, GO commits with ≥1 digit; fixed-width
  zero-padded IDs are gone ("ID: -" shows while empty). ③ **"Reset WiFi" on P0**:
  auto-reconnect from NVS is the intended default, but a wrong saved network had
  no way out — UP/DOWN now selects "> RESET WIFI (OK=confirm)" and OK erases
  `sta_ssid`/`sta_pass` and restarts the AP (new `meta_store_net_reset_wifi()`);
  ONLINE auto-advances to P1 after 2s. ④ **Play 675 "unavailable" root cause**:
  its `rec` partition is a data partition (type=1) with subtype 0x40 (custom),
  which the analyzer's hard-reject rule made uninstallable, and the stale deployed
  server 404s every `/api/analyze` (the firmware renders any non-contract reply
  as "unavailable" — the visible trigger). The analyzer now warns and allows
  subtype-0x40 custom data partitions (`supported=true`, reason=custom-partitions,
  `detail=<label>`; hard reject only when subtype != 0x40), the analyze contract
  gains a `detail` string, and the slot page renders "NOTE: custom 'rec' part /
  not installed; some features may lack it" while still offering CONFIRM.
  **Deployment**: the production worker (`install-slot/_worker.js`) now implements
  the full device channel (analyze/extracted via the same `store-analyze.js` +
  WebCrypto SHA-256, which moved from `tools/install-slot/` into the Pages deploy
  root per the BUG-03 single-canonical-copy rule); deploying = pushing main
  (`wrangler pages deploy` in CI). The previously deployed worker predates the
  store channel entirely — it 404s every `/api/analyze`. Follow-up contract fix in the same cycle: the
  server answers unsupported plays with `name:null` / `extracted:null` (sha256 is
  computed lazily), but the firmware parser treated `name`/`extracted` as always
  required, so too-large/wrong-chip/not-found were swallowed into "format" on
  screen; parsing now requires `name`/`extracted` only when `supported=true`
  (pinned by a new host contract test, `tests/test_store_analyze_contract.c`,
  linked against the same `meta_store_analysis.c` the device runs and fed real
  local-server responses). Factory budget after r8: 1,146,544/1,507,328 B
  (~24% free); release artifact `meta-pass_v1.0.0-8-g2595d26.bin` (1,212,124 B
  incl. MPUPV2 footer), verify gates 4/4 PASS.
- **Store download channel (feat/ota, plan v3.2-r6)**: replaces the SoftAP upload import
  (`meta_net`/`meta_import` and their tests retired) with on-device OTA from the app store via
  metapass.chuanxilu.net — the device's single TLS trust anchor. Flow: WiFi provisioning (SoftAP
  form, credentials persisted in NVS) or saved-credential reconnect → SNTP → numeric play-ID
  keypad → one `/api/analyze` (name / installability / smallest fitting slot; business results
  always 200 + reason codes) → slot select → streamed `/api/extracted` flashed with rolling
  SHA-256 compared against both the analyze summary and the `x-image-sha256` response header,
  then `esp_ota_end`/`esp_image_verify` before the slot is marked valid and the MNAM display-name
  blob written. Unpacking happens server-side (`tools/install-slot/store-analyze.js`, pure ESM,
  byte-identical algorithm to the install page; analyze/extracted share one verified-bytes cache
  keyed by play id with revisionId re-check). Cancel goes through a confirm page (CANCEL / RETRY /
  BACK); the session timeout asks instead of closing (configurable via
  `CONFIG_META_STORE_SESSION_TIMEOUT_MS` / `CONFIG_META_STORE_HTTP_TIMEOUT_MS` or
  `meta_store_session_set_timeout_ms()`). Custom two-root certificate bundle (`main/certs/`, GTS
  Root R4 + ISRG Root X1) replaces the default Mozilla bundle. Host checks: `test_meta_store_json`
  (bounded JSON extractor), 12 store-analyzer node cases, IDF 5.x-signature stub `-fsyntax-only`
  for `meta_store_net`/`meta_store_api`; `validate.sh --static` green. Remaining acceptance
  (plan §5 Phase 4/5): esp_emu end-to-end and on-device OTA. Real-device build re-verified:
  factory budget 0x115d20 (r6) → 0x1178d0 (r7), ~24% free.
- **Provisioning usability (r7)**: the SoftAP hotspot is now open (no password — no more
  copying an 8-char key off the tiny screen) with the SSID still randomized (`metapass-XXXX`);
  the provisioning page gains a "Scan networks" button backed by a new `/api/scan` endpoint
  (periodic background scan, cached results, max 20, JSON-escaped SSIDs, textContent-only
  rendering); a UDP/53 DNS hijack plus catch-all 302 turn it into a captive portal so phones
  auto-open the setup page on connect. `/api/wifi` rejects empty passwords. Plan doc r7 entry
  documents the rationale.

## v1.0.0 (2026-09-18)

First stable release: all four flows — market install, data-preserving
upgrade, slot backup/restore, firmware signing toolchain — are complete;
the four contracts (hybrid single-file MPUPV2, backup manifest v1,
signature format, 3-slot partition layout) are frozen as of this version
(see entries below; the adaptive design keeps future layout tweaks from
breaking existing backups and upgrade paths).

- Boot policy upgraded to **bootloader-enforced (2026-09-18)**: new
  `bootloader_components/meta_boot_hooks/` (IDF hooks mechanism; `bootloader_after_init`
  runs before any application) inspects both otadata copies and erases any whose
  `ota_state == VALID` — a child writing VALID can no longer persist across reboots. The
  boot policy is decided unilaterally by meta-pass, independent of child behavior; devices
  locked by pre-model children self-heal on the next power cycle, no re-flash needed.
  PENDING is untouched (trial-run rollback intact), deep-sleep wake skips everything, and
  intervention is disabled under flash encryption. The policy's pure logic lives in
  `main/meta_boot_policy.h` (host test `tests/test_meta_boot_policy.c` pins the 32-byte
  copy layout and every state decision); QEMU gains case C3: otadata preloaded with the
  harshest resident state (CRC-valid VALID + a real child image in ota_0), asserting both
  copies get erased and the boot falls back to the factory list page.
- Installer backup/restore now carries **NVS automatically (2026-09-18)**: the NVS
  partition (data/nvs subtype, `cardid` excluded) is located from the device's own
  partition table, read automatically during backup and packed as `nvs.bin` (SHA-256 in
  the manifest `nvs` field); restore verifies presence/size/digest and writes it back at
  the target device's located offset (adaptive — no source offset reuse). No user choice
  on either side; an erased NVS is skipped and older zips stay compatible. Rationale: bare
  flashing the single-file image erases NVS at 0x9000 — the backup/restore round-trip is
  the only way app data survives a re-flash. Tests: `test-slot-backup.mjs` PASS 9
  (location rules + manifest compat) and PASS 10 (backup→restore data-path contract with
  tamper negative).
- Single release artifact (build/packaging): `tools/build-firmware.sh` now emits ONE file —
  `meta-pass_v<version>.bin` (~1.1 MB), replacing the three-artifact set (8 MB merged image,
  `meta-pass-bootable_*`, MPUP upgrade container). Format: bootable body (bootloader +
  partition table + phy + app at flash offsets, byte-identical to the merged image head)
  plus a 44-byte `MPUPV2` footer (magic + body length u32le + body SHA-256). Market tools
  flash it raw at 0x0 (ROM boots the body; the footer lands in unused factory tail space);
  the installer's "Upgrade launcher" now accepts this same file — `parseUpgradeArtifact`
  (launcher-upgrade.js) verifies the footer, slices bootloader/table/app out of the body,
  and treats otadata as a generated all-0xFF segment; legacy `MPUPV1` containers stay
  accepted. The 8 MB merged image remains in `build/` for `verify_firmware.py`/QEMU only;
  the verifier rejects stale `bootable_*`/`upgrade` artifacts and enforces footer length /
  body parity. Covered by new PASS 7/8 in `test-launcher-upgrade.mjs` (real-artifact slice
  parity, tamper negatives, legacy compat) and the QEMU harness C1/C2/A2 now boot the
  hybrid file end-to-end (A2 turned from negative to positive — the single file must boot).
- Single-session child boot (launcher): every power-on returns to the launcher list page —
  child firmware no longer persists across reboots. Root cause: the signed child called
  `metapass_mark_valid()` → `esp_ota_mark_app_valid_cancel_rollback()` wrote otadata=VALID
  (flash-persistent), so the bootloader booted the child slot directly on every power-on and
  the launcher never ran; a child that occupies the OK long-press without a return hook would
  lock the device (and a crash-looping child = a true boot loop). Fix, two layers: the hook's
  `metapass_mark_valid()` is now a signature self-diagnostic only (no `cancel_rollback`; ota
  state stays pending → any reboot/power-cycle auto-falls back to factory, crash recovery rides
  the same rollback), and the launcher erases otadata early in `app_main` (`meta_store_mark_factory_valid()`,
  contract updated in `meta_store.h`) maintaining the invariant "launcher ran ⇒ otadata empty ⇒
  next boot defaults to factory". Boundary: a device already held by an old-model resident child
  never reaches the launcher — unlock via that child's return hook (OK long-press) or a re-flash.
  `meta_store.h` comment corrected (`5cbadca`): mark_factory_valid erases otadata, it does not
  mark anything valid. Design docs/README/sdkconfig synced to the single-session contract.
- sign-firmware.sh now accepts the full merged image (bootloader + partition table + app,
  the marketplace flashable format) in addition to bare app images. Root cause fixed:
  the script parsed the bootloader header as the app header (image_len=21024, negative
  `total`, signature at an offset the device never reads -> "unsigned" on device even
  though the command included --egg-text). App location now comes from the single-source
  locator `tools/signing/locate_app_image.py` (factory partition @0x10000, same contract
  as install-slot/extract-app-image.js); merged outputs keep bootloader/partition bytes
  byte-for-byte and append pad + 4 KB metadata sector only; digest covers the app region
  only. test_integration.c gained the same merged-image parsing. Output labels clarified
  (`total` = signed output file size; `sig_offset` now printed slot-relative + absolute).
  Verified on-device-representative host tests: merged + bare inputs both PASS
  (META_SIG_OK + egg parse), structure asserts (head preserved / pad 0xFF / MSIG@offset / MAEG xor).
- Revert pipeline window 32768 -> 64 (stop-and-wait, identical to esptool.py read_flash);
  keep 921600. On-device A/B: esptool.py stop-and-wait @921600 ran 5/5 clean (87 KB/s),
  while the self-built pipeline failed proportionally to throughput at the same baud.
  Mechanism: with pipelining, ACK uplink overlaps the bulk data downlink on the same USB
  CDC endpoint, triggering C3 USB-Serial-JTAG RX loss (log evidence: post-failure drains
  up to 16 s = the stub was sitting on a large backlog of in-flight data). 921600 already
  shrank the per-frame gap from ~300 ms to ~10 ms, so pipelining bought <=15% at high risk.
- Post-hoc retraction of the round-6 investigation doc: the claimed standalone 2-byte
  stub error/status frame does not exist (header + status share one SLIP frame per
  stub_flasher.c; the OK probe and successful full backups disprove a leftover frame).
  Kept: the chunkT0 scope bug (real) and the mock-fidelity methodology lesson. See
  backup-readflash-error-status-frame.md (zh_CN keeps the original as an archive).
- On-device CLI verdict (esptool.py 4.12, 128 KB from slot-0 base): 115200 = 11.4 s,
  921600 = 1.5 s (7.6x); SHA256 identical across both baud rates; 5 consecutive reads
  at 921600 all passed. The high-baud link itself is reliable — backup read failures
  were client-side recovery flaws (now covered by tiered recovery), not the link.
- Tiered read-recovery for backup desync: L1 soft recovery (re-ACK, drain, sync) ->
  L2 reopen the port at the session baud (fixes the hardcoded 115200 reopen that caused
  "5 consecutive failures at the same address": during a 921600 session a 115200 reopen
  yields baud-mismatch garbage) -> L3 full USB-JTAG reset + stub re-upload + baud
  restore (fresh loader instance; no reuse of half-dead state). Every tier logs and
  propagates its own failures — no silent hangs. Round-7 hardening (source-verified against
  `stub_commands.c`): the L1 recovery ACK was 0x8000, which only aborts the stub's
  `handle_flash_read` when `num_acked >= num_sent` — with fewer bytes in flight the stub kept
  streaming the remainder and re-poisoned the line (same-address retry cascades). The recovery
  ACK is now 0xFFFFFFFF (≥ any num_sent → deterministic abort → digest → command loop),
  drain silence 300→800 ms (digest + in-flight residue need a wider window), vendor data-frame
  timeout 8 s→1.5 s (a 4 KB frame is 44 ms at 921600), recovery sync 8 s→1 s, retries 5→8
  (p⁸ ≈ 1e-4 per chunk), i18n retry count follows the constant. Expected effect: transient
  per-chunk failures still appear in the log (device-side, unavoidable) but each costs ~4 s
  instead of ~10 s and no longer cascades into slot-wide abandonment.
- Connect at 921600 baud (8x faster reads). The earlier conclusion that "baudrate is a
  no-op on C3 native USB" was disproved by measurement: debug logs show ~356 ms per 4 KB
  frame, matching 115200-baud wire time. The reason previous baud changes did nothing:
  the page passed baudrate===romBaudrate, so the vendor main() changeBaud branch never
  fired. Now connecting with baudrate=921600/romBaudrate=115200 (standard changeBaud flow),
  with an immediate data-path check and automatic fallback to 115200 on any failure
  (worst case = previous behavior). Read timeout 15s->8s.
- Pipelined backup reads for a large speedup (measured 10.1 KB/s baseline): pinned down the
  `max_in_flight` semantics of the stub's `handle_flash_read` (`stub_commands.c:111`,
  `num_sent - num_acked < max_in_flight` — all three are **bytes**). We passed 64, i.e. 64
  bytes — less than one 4 KB frame, so the stub stopped and waited for an ACK after every
  frame; on top of that, USB-CDC only flushes a trailing partial (<64 B) packet when pushed
  by the next data wave — and in stop-and-wait every frame ends in a 4-byte trailing packet,
  costing ~300 ms per frame. The in-flight window is now
  `globalThis.__READFLASH_PARAMS__ = [4096, 32768]` (byte window = one 32 KB chunk; the stub
  streams 8 frames before waiting); ACKs are still sent per frame (as esptool.py does), with
  cumulative values staying within 0x1000..0x8000 (no 0xC0/0xDB bytes). Covered by a new
  "window bytes semantics" case in `test-readflash-protocol.mjs` (32 KB read must stream 8
  frames with zero ACKs; stop-and-wait mode still supported).
- Fix two transport-layer deadlock defects (observed as: every read session dies at ~2 min,
  and the retry recovery then hangs silently for 30+ minutes with no log):
  ① vendor `readLoop` abandons the in-flight `reader.read()` on timeout; when that
  generator's timeout timer later fires, `finally{buffer=new Uint8Array(0)}` wipes the whole
  transport buffer — any read session outliving `FLASH_READ_TIMEOUT` self-destructs, and
  every `newRead` spawning a fresh generator left stale timers counting. Now: persistent
  `_pendingRead`, explicit generator close, buffer wipe removed; `FLASH_READ_TIMEOUT`
  100s→15s.
  ② vendor `flushInput()` starts with `await this.reader.closed`, which never settles on an
  active serial port — the recovery path hung there forever. Now a bounded cancel
  (cancel + 500ms race); the page-level recovery chain got hard timeouts on every step and
  auto re-opens the serial port when sync fails.
- Add a Debug-mode checkbox to the backup section: logs protocol-level diagnostics (per-chunk
  timing, recovery steps, timeout positions) for remote troubleshooting.
- Fix the root cause of backup read failures ("Packet content transfer stopped" / "No serial
  data received", retries never recovering): the esptool stub appends an unconditional 16-byte
  MD5 digest frame after flash-read data frames (`stub_commands.c`), which esptool-js never
  reads — the leftover frame poisons the next command's response and protocol desync
  accumulates per `readFlash` call. `install-slot/vendor/esptool-js.js` now ACKs per data
  frame and reads/verifies the digest frame (matching `esptool.py read_flash`); new
  `install-slot/vendor/md5.js` provides the digest check; read params pinned to the official
  values (4 KB block — the stub's hard limit — and 64-frame in-flight window). Covered by
  `tools/install-slot/test-readflash-protocol.mjs` (mock-stub protocol test, wired into
  `validate.sh`).
- Add `tools/test-bootable-qemu.mjs`: headless QEMU boot verification — boots build artifacts
  through the passport-sim QEMU WASM core and asserts three things: the UART0 test variant
  completes bootloader → partition table → factory app → app_main; the MPUP upgrade container
  flashed raw at 0x0 indeed fails to boot (negative case, matching real-device behavior); the
  market image (USB-JTAG config) renders non-black ST7789 frames (LVGL display init runs).
  The bootability of the market image `meta-pass-bootable_*.bin` now has automated evidence
  instead of relying on real-device trial flashes.
- Fixed the installer connect flow breaking after a failed connect: the serial port is now released on any connect error (previously a hung or failed connect left the port open and every retry hit "The port is already open"), connects are non-reentrant (`busy`/`connecting` guards), a half-open session no longer clobbers an existing connection (commit to globals happens only after every step succeeds), and a device that silently drops a command no longer hangs the flow forever (15 s probe timeout via `withTimeout`). Removed the pointless `changeBaud()` disconnect/reconnect dance — baud is a no-op on the C3's native USB — and replaced the broken 16 KB read-block probe (the stub's `handle_flash_read` uses a 4 KB stack buffer and **silently returns** for larger blocks) with the officially sanctioned speedup: block size stays at the stub's 4 KB maximum while the in-flight window is raised from 4 KB to 64 blocks × 4 KB = 256 KB, cutting ACK round-trips 64× (mirrors upstream `esptool.py` which already used 4 KB blocks / 64-deep window). Probe verifies bootloader magic + exact length and falls back to conservative 1 KB/4 KB on any mismatch.
- Data-preserving launcher upgrades (§7.2): upgrades write only bootloader + partition table + factory app + an erased-state OTA-data reset; NVS (Wi-Fi credentials), `cardid`, and all three child-firmware slots are never touched. The USB installer gained a "7. Upgrade launcher" section that reads back the device partition table and byte-compares it with the bundle before writing (layout mismatch refuses the upgrade). `tools/build-firmware.sh` now emits a `build/upgrade/` bundle (4 files + `flash-args.txt`), and `tools/verify_firmware.py` enforces that the merged image keeps `nvs`/`ota_0-2`/`otadata` erased so a full image can never carry user-data-destroying content. Core logic in `install-slot/launcher-upgrade.js` with Node tests wired into `tools/validate.sh --static`. The upgrade is distributed as a **single-file MPUP container** (`build/upgrade/meta-pass-upgrade_<version>.bin`: magic + segment table + per-segment SHA-256) — the installer picks this one file, unpacks and verifies it, then writes the four segments to their partition addresses; `verify_firmware.py` additionally enforces container parity with the merged image.
- Slot backup & restore on the USB installer page (`install-slot/`): per-slot backup reads the full partition, slices it into `slot{N}_firmware.bin` (parsed ESP app image) + `slot{N}_tail.bin` (4 KB MSIG/MAEG/MNAM metadata sector) + optional `slot{N}_extra.bin` (post-tail storage), computes SHA-256 per file, and packs everything with a `manifest.json` into a timestamped zip. Restore lets the user map each backed-up slot to any target slot, checks free space against the manifest lengths (adaptive to future slot-size changes), verifies every file's SHA-256 before writing, then flashes firmware → extra → tail (tail last so sector re-erase cannot destroy earlier writes). A slot containing data that is neither erased nor a recognizable app image (e.g. slot 2 doubling as a data-storage partition, littlefs volumes, non-ESP resource packs) is no longer skipped: it gets a dd-style raw mirror fallback — `slot{N}_raw.bin` with trailing erased bytes trimmed, a `type: "raw"` manifest entry, restore written back from the slot base address with only a size ≤ partition and SHA-256 check (no tail-sector reservation). Backup/restore live in their own sections (§5/§6) separate from the install flow; core logic ships in the pure ES module `slot-backup.js` with Node tests wired into `tools/validate.sh --static`. Fixed a `DataView(TypedArray)` compatibility bug in the zip reader (older engines require an ArrayBuffer).
- Signature-verification hardening (`feat/sign` branch): fixed BUG-01/02/04 (uninitialized battery label, inverted `HOST_TEST` egg-magic check with m1–m4 regression tests, `%zu` for `size_t` log), single-source install page (`server.mjs` serves the canonical `install-slot/`; dev-copy drift closed, BUG-03), one-command local build (`tools/build-firmware.sh`), bilingual bug report and root-cause knowledge base (`docs/BUGS.md`, `docs/assets/handoff-unsigned-rootcause.md`, `docs/assets/meta-pass-signing-design.md`, `docs/development/engineering/debugging-workflow.md`). Root cause of the on-device "unsigned" symptom: the stale deployed installer page, not the signing chain; re-flashing through the fixed page restores expected behavior.
- Added the meta-pass multi-firmware launcher (`feature/meta-pass` branch): the partition table keeps the `factory`/`cardid` contract in place while adding `otadata` and three OTA slots of differing sizes (`ota_0@0x180000` / `0x1D6000`, `ota_1@0x360000` / `0x200000`, `ota_2@0x560000` / `0x29E000`); app rollback is enabled (un-adapted child firmware automatically falls back to the launcher on any reboot); firmware import over Wi-Fi SoftAP + web page (random password + on-screen one-time pairing code, streamed in 1024-byte chunks); mandatory image integrity checks (magic/chip-id/size/SHA-256 display, with `esp_ota_end()` as the authoritative re-check) and a warning page with a BOOT / CANCEL menu before booting unsigned firmware; local management UI to list/boot/delete slot firmware; the button BSP exposes an explicit `BSP_BTN_LONG` (1.5 s) threshold; pure-logic modules (image validation, slot registry, import state machine) ship with host tests wired into the static gate. Design: `docs/assets/meta-pass-design.md`.
- Second import channel (USB serial, `tools/install-slot/`): Chrome + Web Serial +
  esptool-js write child firmware directly into a slot while the device is in ROM
  download mode (power on while holding UP); sources are a local `.bin` (Full Flash
  images are unpacked to their app image) or a community play link (verified against
  the published SHA-256). Design: `docs/assets/meta-pass-design.md` §6.1.
- Slot display-name blob (§6.2): the real firmware name is written at install time
  into the slot partition's last 4KB sector (`slot_offset + partition_size − 4KB`,
  derived per slot because the three slot sizes now differ: `0x1D6000`/`0x200000`/
  `0x29E000`; `magic "MNAM"` + length + printable ASCII + XOR checksum, ≤32 bytes);
  the launcher scan prefers it and falls back to the core name (`project_name` minus
  the `FoloToy-` prefix, new `meta_slot_core_name`). `ota_2` is a dual-use area
  (bootable child slot, or littlefs recording storage when empty). The factory
  app limit tightens to 1.44 MB (`0x170000`) with `ota_0` moved into the
  cardid-before gap (`0x180000`). The USB install page auto-fills the community
  play's English title / local file name; the Wi-Fi import page gained an optional
  name field.

- Added the supplied 80-byte CW2017 profile for the specified 520 mAh cell, including content/update-flag checks, verified writes, the required restart sequence, and bounded SOC-readiness polling.

- Expanded the environment bootstrap document: added Espressif's Git service mirror (`git.espressif.com.cn`) as the preferred mainland-China route for ESP-IDF v5.5.3 and its submodules, documented submodule long-wait/timeout handling, in-place repair, and the pinned-commit shallow fetch for large submodules such as `esp32-wifi-lib`, warned about stale per-repository Jihulab `insteadOf` residue, and added the official offline release archive as a last-resort fallback (learned from `esp-mosaico/esp-mosaico-vibe`).

- Reorganized the documentation by function area with a dual entry point: the root `AGENTS.md` is now a thin router (hard constraints + task routing only) and the detailed AI workflow lives in `docs/development/ai-guide.md`; `agent-guide.md` was folded in. `docs/development/` gained a second level (`engineering/`, `ci/`, `release/`), and the `plays/` application archive and `experiences/` moved into a `docs/reference/` area with a dedicated README. Removed `docs/software-design/` (empty scaffold); folded the three `assets/{fonts,images,music}/README` leaves into the `assets/` README; flattened the six `project-completion` sub-documents into a single file; and unified each directory to a single README, eliminating every `INDEX` file and a duplicated experience index. All cross-references and bibliographic links were updated; no content was dropped.

- Removed the obsolete app/test partition at `0x700000` and its related
  bootloader, validation, and documentation requirements. The fixed protected
  `cardid` partition and its CI checks remain unchanged.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
