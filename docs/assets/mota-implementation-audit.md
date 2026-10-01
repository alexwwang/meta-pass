[简体中文](mota-implementation-audit.zh_CN.md) | English

# feat/mota Implementation Audit (r10.22 / r10.22b)

Audit of the LAN phone-assisted install implementation on `feat/mota` against the
approved design `docs/assets/lan-pair-install-design.md`. Scope: commits
`0c004a9` (design) → `aaf1b3a` (device cutover) → `f6c8bcc` (phone web module) →
`859c4af` (cleanup). Method: two independent reviewer passes (device firmware;
phone/Worker), live production probes against `https://metapass.chuanxilu.net`, host
test suite (`./tools/validate.sh --static`), and direct code inspection of ESP-IDF
v5.5.3 semantics where concurrency claims depend on them.

Fix status legend: ✅ fixed in this round · ⬜ not fixed (deferred, reason given).

## Verdict

The protocol/state-machine layer matches the design closely and several details are
better than specified (see "Better than design"). It is **not merge-ready**: two
independent phone-side defects each fully break the user flow, and the device-side
UI-cancel path can free a live OTA handle mid-write. All findings below are
evidence-backed; none is speculative.

## Blocking findings

### B1. boot() uploads the merged image instead of the extracted app image — ✅

- Where: `install-slot/phone-install.js:534` (auto-mounted UI path).
- What: `install()` calls `runInstall(bridge, pre.offer, pre.merged, …)` but
  `runInstall` enforces `upload.length === offer.imageLen` (the extracted factory app
  length, `phone-install.js:386`). `pre.merged` is the full merged image
  (bootloader + partition table + app), so the guard always trips with
  `upload buffer missing/length mismatch`. Reproduced with a stub bridge.
- Why tests were green: every test passes `pre.ext.data` directly to `runInstall`;
  nothing exercises `boot()`/`install()`, the only path real users hit. Every install
  attempt from the device QR page would fail *after* the user's physical slot
  confirmation.
- Fix: pass `pre.ext` (runInstall accepts the `{data}` shape) — one line; add a
  boot-path regression test.

### B2. getPlayDetail calls `/api/plays/id/<id>`, which the Worker never routes — ✅

- Where: `install-slot/phone-install.js:206`.
- What: the Worker routes exactly `/api/plays` (`_worker.js:211`) and
  `/api/play?id=` (`_worker.js:213-216`); `/api/plays/id/<id>` falls through to
  `env.ASSETS.fetch` → Pages 404. Live-verified:
  `GET /api/plays/id/563` → 404, `GET /api/play?id=563` → 200.
- Consequence: `preflight()` step `market` always fails in production; the flow never
  reaches download/verify/upload.
- Fix: phone calls `/api/play?id=<id>` (Worker already proxies it to the upstream
  detail endpoint); align the host-test mock with the real Worker route table.

### B3. Worker `/api/plays` proxy drops the entire query string — ✅

- Where: `install-slot/_worker.js:211`; same bug in `tools/install-slot/server.mjs:172`.
- What: `proxy("/api/plays")` fetches `BACKEND + upstreamPath` without `url.search`.
  Live-verified: `/api/plays?q=<nonsense>` returns the identical 673-play unfiltered
  list as no query; the same `q` directly against the backend returns 0 plays.
  Design §4.3 item 1 requires forwarding the supported parameters, especially `q`;
  §11 acceptance "search by Chinese/English keywords returns official-market results"
  fails. `worker_contract.mjs` cannot catch this — it greps source text for endpoint
  strings, it does not execute routing.
- Fix: `proxy("/api/plays" + url.search)` in the Worker; same in `server.mjs`.

### B4. UI cancel races in-flight chunk write: `esp_ota_abort` frees a live OTA handle — ✅

- Where: `main/meta_store_install.c:552-565` (`meta_install_cancel`), invoked from the
  button/UI task (`main/main.c:1153-1155`); racing target `meta_install_chunk_write`
  (`meta_store_install.c:400-440`) on the httpd task.
- What: cancel runs `offer_and_upload_clear()` → `esp_ota_abort()` +
  `mbedtls_sha256_free()` with no synchronization against a concurrent
  `esp_ota_write()` / `sha256_update()` on the same handle/context. Verified against
  local ESP-IDF v5.5.3: `esp_ota_abort` (`esp_ota_ops.c:438-448`) does
  `LIST_REMOVE` + `free()` with no lock → in-flight write dereferences a freed
  `ota_ops_entry_t` (heap use-after-free). The phone-initiated `/api/install/cancel`
  is safe (same httpd task as the chunk handler); `meta_install_net_stop()` is mostly
  safe (`httpd_stop()` drains the in-flight handler first). The device-UI OK-LONG
  cancel during an active upload is the unprotected path.
- Fix: serialize the UI-task entry points (`cancel`/`token_stop` and the session
  state they clear) with the httpd-side upload path via a session mutex.

### B5. Cancel during the `esp_ota_begin` erase window leaves the slot destroyed but not INVALID — ✅

- Where: `main/meta_store_install.c:407-424`.
- What: `flash_touched` is set only after `esp_ota_begin` returns, but `begin` with a
  nonzero length synchronously erases `ALIGN_UP(image_len, erase_size)` (IDF v5.5.3
  `esp_ota_ops.c:189-197`) — a multi-second window for MB-scale slots. A cancel (or
  store exit) in that window sees `flash_touched==false`/`ota_open==false`, aborts
  nothing and marks nothing; when `begin` returns, the handler resumes writing and
  flips state from `cancelled` back to `uploading` (`:436-437`). Net: old firmware
  erased/partially overwritten, registry still says VALID until the next boot rescan,
  UI shows a resurrected upload after an explicit cancel. Design §6.5/§8 mandate the
  INVALID marking.
- Fix: set `flash_touched = true` **before** calling `esp_ota_begin`; fold into the
  B4 mutex so cancel cannot interleave with begin/write at all.

## Non-blocking findings

### M1. runInstall reports success when the device never reaches `done` — ✅

- Where: `install-slot/phone-install.js:437-445`.
- What: after finalize returns 200, the completion poll is bounded to 30 s; on
  deadline `pollUntil` fabricates `state:"timeout"`, and a thrown status error is
  swallowed to `null`. Both paths fall through to `{ok:true, slot}` — a device that
  accepted finalize but then stalls (or a dark status endpoint) is reported as a
  successful install.
- Fix: treat `timeout`/unreadable done-state as
  `{ok:false, stage:"finalize", reason:"device did not reach done"}`.

### M2. Session resume at exactly `imageLen` is rejected instead of skipping to finalize — ✅

- Where: `install-slot/phone-install.js:378-383`.
- What: when the device reports `offset === offer.imageLen` (a prior upload completed
  but finalize never issued — exactly the §6.5 "retry starts at device-reported
  offset" scenario), runInstall fails with `device offset … already beyond image
  length`. The user cannot complete such an interrupted install without cancelling on
  the device.
- Fix: accept `offset === imageLen` and jump to finalize; reject only `offset > imageLen`.

### M3. Worker `/api/firmware` buffers the whole merged image instead of streaming — ✅

- Where: `install-slot/_worker.js:100-119` (`proxy()` does `await upstream.arrayBuffer()`).
- What: every multi-MB merged image is materialized in isolate memory before the first
  byte reaches the phone; adds TTFB and risks isolate memory limits under concurrent
  downloads. Design §4.3 item 4 explicitly requires streaming. The local dev server
  already streams correctly (`server.mjs:94-124` pipes with backpressure), so the
  Worker is the outlier.
- Fix: `return new Response(upstream.body, …)` forwarding content-type and
  content-length.

### M4. Version-skew window: no-store entry module imports 4h-cached unversioned modules — ✅

- Where: `install-slot/_worker.js:508-515`.
- What: `/phone-install.js` is served `cache-control: no-store`, but its static imports
  `./extract-app-image.js`, `./store-analyze.js`, `./name-blob.js` fall through to
  `env.ASSETS.fetch`, which the live deployment serves as `public, max-age=14400`
  (curl-verified on all three). After each deploy a phone can load a fresh
  phone-install.js importing up-to-4-hour-old dependencies — a cross-module mismatch
  the device protocol handshake cannot detect. Design §4.3 item 5 allows immutable
  caching only for content-addressed or versioned assets.
- Fix: route the three imported modules through the same no-store path as
  phone-install.js.

### M5. Device `meta_install_finalize` ignores MNAM display-name write failure — ✅

- Where: `main/meta_store_install.c:523-540`.
- What: the MNAM tail-sector write failure is only logged; the function continues and
  returns `ESP_OK` after the registry already marked the slot VALID — the slot passes
  verification but shows no display name. Design requires the display-name blob write
  to succeed.
- Fix: MNAM write failure → finalize fails (slot INVALID via the existing
  `flash_touched` path).

### M6. No upload inactivity timeout: stalled phone pins the store session forever — ✅

- Where: `main/main.c:673-675` (`store_tick`).
- What: `state=="uploading"` latches and renews the store deadline every 250 ms tick;
  nothing keys renewal off actual chunk arrivals. If the phone disappears mid-upload
  (browser closed, WiFi roamed), no more chunks arrive, the deadline never fires, and
  the device holds WiFi + install httpd + an open OTA session indefinitely. Design
  §6.5 specifies "interruption leaves the session open until timeout or explicit
  cancel" — the timeout half is unimplemented for a stalled upload.
- Fix: renew on upload activity (chunk arrival / session open), not on the latched
  state; add a stall threshold (e.g. 30 s without a chunk) after which the normal
  store-deadline path runs.

### M7. Late prepare can erase a completed physical slot confirmation — ✅

- Where: `main/meta_store_install.c:773-809`.
- What: `h_install_prepare` checks `confirmed || session_opened` once at entry, then
  blocks up to ~10 s in `req_body()`, then unconditionally clears and installs the new
  offer. If the user physically confirms during that window, the prepare completion
  silently erases the confirmation: the UI sits on the upload page forever, and the
  phone's `session` call fails 409 "not confirmed". Deadlock until session timeout or
  manual exit.
- Fix: re-check `confirmed || session_opened` after the body read (inside the B4
  mutex) and reject with 409.

### M8. Detail view omits `updatedAt` required by the design metadata contract — ✅

- Where: `install-slot/phone-install.js:525-531` (`showDetail`).
- What: `normalizePlay` preserves `updatedAt` (`:184`) but the detail render shows
  only name/size/revisionId/sha-prefix. Design §5/§6.2 and §11 require update time on
  the detail page.
- Fix: one-line UI addition.

### M9. Stale references to removed WAN modules in comments/config — ✅

- Where: `main/meta_store.h:20-24` (consumer named `meta_store_api`, actual consumer
  is `meta_store_install.c`); `sdkconfig.defaults:27-29` (justifies INFO logging via
  `esp_http_client`/`esp-tls` diagnostics from removed components) and
  `sdkconfig.defaults:109`-area section header (Chinese: "store download channel
  (TLS + SNTP)") over what is now LAN-tuning config.
- Fix: comment-only corrections. No behavior change.

## Better than design (kept as-is)

- Pair code concretized: 6 digits / 5 min TTL / 5 tries, token lifecycle bound to the
  store session; `meta_install_token_stop` invalidates the slot if flash was touched.
- Chunk upload rejects chunked-transfer bodies (`content_len==0`) instead of letting
  httpd buffer implicitly.
- Strictly ordered state machine: prepare→confirm→session→chunk→finalize with no
  out-of-order transitions; `flash_touched` prevents resume across reboots.
- Resume semantics correct: device offset is the single source of truth; half-written
  chunks resume from the device-reported offset; in-place retry capped at 3.
- Phone preflight triple gate (size / store sha / analyze-vs-local extract) fully
  implemented; pure-JS SHA-256 pinned against node:crypto on 9 vectors.
- `meta_store_json_get_array_*` helpers accept flexible partial slot arrays.

## Test-suite gaps closed in this round

- The metapass mock in `tests/test_phone_install.mjs` implemented
  `/api/plays/id/<id>` (mirroring the phone module's expectation) instead of the
  Worker's real route table — the B1/B2 class of bug passes green. The mock now
  mirrors the real Worker routes and asserts `q` forwarding.
- `tests/worker_contract.mjs` is source-text matching; added executable-style gates:
  query forwarding on `/api/plays`, streaming `/api/firmware` (no `arrayBuffer` in the
  proxy), no-store on all phone-module imports, phone `getPlayDetail` path matches a
  Worker route.
- New regression tests: boot-path payload shape (merged vs extracted), resume at
  exactly `imageLen`, finalize done-poll timeout → failure.

## Deployment note (not a code fix)

`/phone-install.js` returns 404 on the live deployment because `feat/mota` is not
merged/deployed yet (Worker route + ASSETS bundle both ship from `main`). Merge +
`workers.yml` deploy makes B1-B4/M3/M4 reachable in production. Until then the live
probes above document the pre-fix state.

## Verification

- `./tools/validate.sh --static` — PASS after fixes (host tests incl. the new
  regression cases).
- Firmware rebuild not re-run in the audit shell (local ESP-IDF toolchain missing
  xtensa components); the firmware delta is confined to `meta_store_install.c` /
  `main.c` and is covered by the host model tests + stub syntax checks. Rebuild before
  tagging.
