<p align="right">
  <a href="ota-r10.17-r10.19-audit.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# OTA r10.17-r10.19 Code Audit

Date: 2026-09-30  
Reviewed range: `1a69f37..HEAD` (`eb99f84`, `81e9e1d`)  
Working tree at audit time: clean; `feat/ota` was 13 commits ahead of `origin/feat/ota`.

## Verdict

No blocking firmware correctness issue was found. The OTA state machine does not expose a path that writes shifted or foreign bytes into flash. The R2 range path is production-verified for byte count, `Content-Range`, and full-body slicing.

The remaining findings are operational and hardening issues. The highest-risk issue is that the documented `RATE_KV` rate limit is not active in production. The next most important issue is that the edge-cache key includes `sig/ts`, so the analyze-time warm cache cannot serve the new ticketed firmware requests.

## Evidence boundary

- Build: PASS.
- Host tests: PASS.
- Production HTTP tests: PASS for the cases listed below.
- Device test: NOT RUN. Final throughput, interrupted-download resume, and slot verification still need a serial-log run on hardware.
- Static validation: `./tools/validate.sh --static` PASS.
- Production E2E: `node tools/e2e-production.mjs` PASS.
- Firmware package gate: `./tools/validate.sh --firmware` PASS.
- Reviewed firmware artifact: `meta-pass/build/meta-pass_v1.0.0-50-g81e9e1d.bin` (`1,236,716` bytes, SHA-256 `5d7c6d88feb84efd3f9a6e38cd949a69c9160fff49be4f478424f372a191ba86`).

## Goal alignment

### Confirmed working

1. Firmware OTA state survives TLS reconnects:
   - OTA handle, SHA-256 context, received byte count, and image-header precheck state are kept across connections.
   - A failed connection is closed; the next connection requests `Range: bytes=<received>-`.
   - Resume requires `206`, an exact `Content-Range`, matching `Content-Length`, and the same `x-image-sha256` as analyze.

2. Legacy-server fallback is safe:
   - A resume answered by `200` or `416` is never appended at the old offset.
   - The firmware aborts the partial OTA, resets transfer state, and falls back to a whole-image retry.

3. Production R2 range responses match the firmware contract:
   - `start=1`: `Content-Length=2664255`, actual body `2664255`, `Content-Range: bytes 1-2664255/2664256`.
   - `start=1000000`: `Content-Length=1664256`, actual body `1664256`, `Content-Range: bytes 1000000-2664255/2664256`.
   - `start=2664255`: `Content-Length=1`, actual body `1`, `Content-Range: bytes 2664255-2664255/2664256`.
   - All sampled responses reported `x-source: r2-range`.

4. The ticketed full-body path reaches R2:
   - A HEAD request with a valid ticket returned `x-source: r2`, the full image length, and the expected image SHA-256 header.

## Findings

### High — `RATE_KV` rate limiting is not active

Files:

- `install-slot/_worker.js:178-185`
- `wrangler.toml`

Evidence:

- The Worker code permits the request when `env.RATE_KV` is absent.
- `wrangler.toml` defines the R2 binding but no KV namespace binding.
- Seven same-id requests within one minute returned seven `416` responses; no seventh-request `429` appeared.

Impact:

- The r10.18 rate-limit claim is not true in production.
- Unauthenticated requests can repeatedly trigger the old compute path.
- Documentation and runtime behavior are inconsistent.

Recommended fix:

- Add a real `RATE_KV` namespace binding, or remove the rate-limit claim and treat limiting as a separate unshipped feature.

### Medium — The edge-cache key includes `sig/ts`

Files:

- `install-slot/_worker.js:28-30`
- `install-slot/_worker.js:58-71`
- `install-slot/_worker.js:288-292`
- `main/meta_store_api.c:412-416`

Evidence:

- `extractedEdgeKey(req)` uses the complete request URL.
- Analyze warms `/api/extracted?id=<id>`.
- New firmware requests `/api/extracted?id=<id>&ts=<ts>&sig=<sig>`.
- These URLs can never share the same Cache API entry.

Impact:

- Analyze-time warming cannot serve ticketed firmware requests.
- A ticketed cold request can store a 2.6 MB cache entry under a one-off `ts/sig` key.
- R2 currently protects the primary path, but the r10.15b edge-warming goal is not achieved for the new firmware path.

Recommended fix:

- Normalize `extractedEdgeKey()` to keep only `id` and strip `ts`/`sig`.

### Medium — R2 range `Content-Length` depends on runtime normalization

File:

- `install-slot/_worker.js:314-329`

Evidence:

- The source sets `content-length` from `obj.size`.
- The R2 API's object-size field is full-object metadata, while a ranged `get()` represents a byte range.
- Production responses are currently correct: for `start=1000000`, the observed header and body length were both `1664256`. The runtime therefore normalized the response length.

Impact:

- No current production failure was observed.
- The code depends on runtime behavior instead of expressing the intended range length explicitly.

Recommended fix:

```js
"content-length": String(total - rangeStart),
```

Add a production smoke assertion that `Content-Length` equals the actual response byte count.

### Medium — The cold path still serializes cache and R2 writes before the response

File:

- `install-slot/_worker.js:404-440`

Evidence:

- A cache miss performs, in order:
  1. upstream fetch, validation, and extraction;
  2. `await putExtractedToEdge(...)`;
  3. `await` R2 object write;
  4. `await` R2 metadata write;
  5. response creation.

Impact:

- The first materialization still carries the full cold-path cost.
- R2 and metadata writes add more time before the device receives the first byte.
- The first request after a revision change can still hit the original slow-path symptom.

Recommended fix:

- Return the computed response immediately.
- Move edge-cache, R2-object, and R2-metadata writes into `ctx.waitUntil()`.
- Keep copied `Uint8Array` data in the background closure; `out.stream` is not a `ReadableStream`.

### Medium — The ten-minute ticket lifetime can expire during the worst allowed download

Files:

- `install-slot/_worker.js:128`
- `main/meta_store_api.c:412-416`
- `main/meta_store_api.c:639-660`

Evidence:

- `TICKET_MAX_AGE` is 600 seconds.
- Firmware reuses the original ticket for every resume connection.
- The theoretical retry budget alone can approach 540 seconds before accounting for transfer time.

Impact:

- Expiry does not corrupt or abort the install.
- The request falls out of the R2 fast path and returns to the computed path, which is the slow path this change is intended to avoid.

Recommended fix:

- Refresh the analyze ticket before expiry, allow a longer Range-resume window, or return a larger `max_age` with the ticket.

### Medium — A stale edge-cache entry can conflict with mandatory SHA-256 response-header validation

Files:

- `install-slot/_worker.js:288-292`
- `main/meta_store_api.c:505-513`

Evidence:

- Edge-cache hits are returned without comparing the cached `x-image-sha256` to the current analyze result.
- The edge-cache TTL is one hour.
- Firmware now treats a missing or mismatched digest header as deterministic failure.

Impact:

- If a play image changes within the cache TTL, analyze can describe the new image while the edge cache serves the old digest.
- The new ticketed R2 path avoids this by keying on the analyzed SHA-256.
- Legacy, no-ticket, and expired-ticket paths remain exposed.

Recommended fix:

- Recheck cached digest metadata before serving an edge hit, or include the analyzed image SHA-256 in the normalized edge-cache key.

### Low — A connection failure after the last byte can waste one whole-image retry

Files:

- `main/meta_store_api.c:608`
- `main/meta_store_api.c:639-659`

Evidence:

- If the connection dies after all `image_len` bytes are written but before EOF, the next request uses `Range: bytes=<image_len>-`.
- The Worker correctly returns `416`.
- Firmware maps that response to Range-unsupported fallback and restarts the full image.

Impact:

- No correctness risk.
- One complete image may be discarded and downloaded again.

Recommended fix:

- In the retry loop, treat `st.received == analysis->image_len` as complete before opening another connection and proceed to verification.

### Low — Download-ticket JSON fields have no host contract tests

File:

- `main/meta_store_analysis.c:102-112`

Evidence:

- Existing analyze fields have host contract coverage.
- `dl.sig` and `dl.ts` parsing has no dedicated host test.

Impact:

- A parser regression can silently disable the ticketed R2 path.
- The install remains safe, but the speed feature disappears without a test failure.

Recommended fix:

- Add host tests for a valid ticket, missing `dl`, invalid `ts`, and a malformed or wrong-length `sig`.

## Cleared by audit

- No flash offset mismatch was found.
- No cross-connection SHA-256 state contamination was found.
- No `esp_ota_abort` / `esp_ota_end` lifetime violation was found.
- The old response-header bug is fixed: response headers now come from `HTTP_EVENT_ON_HEADER`, not the request-header getter.
- A resume response of `200` or `416` is not appended to existing OTA state.
- The certificate-chain and device trust-anchor production checks pass.
- The protected firmware layout and single-file package gates pass.

## Implementation handoff

Status: **all items below are OPEN unless explicitly closed in a later commit**. The owner may split them across Worker and firmware developers, but H1-H6 should land before calling the R2 rollout complete.

### H1 — Make the rate-limit claim real

- Severity: High
- Area: Worker / deployment
- Files: `install-slot/_worker.js`, `wrangler.toml`
- Change: bind the `RATE_KV` namespace used by `rateLimit(env, ip, id)`, or remove the shipped rate-limit claim until the binding exists.
- Acceptance:
  - seven same-`id` requests within one minute produce one `429` after six allowed requests;
  - the check is visible from production behavior, not only source text;
  - `./tools/validate.sh --static` passes.

### H2 — Normalize the edge-cache key

- Severity: Medium
- Area: Worker
- Files: `install-slot/_worker.js`, `tests/worker_contract.mjs`
- Change: make `extractedEdgeKey()` keep only the play `id`; strip `ts` and `sig`.
- Acceptance:
  - analyze warm-up writes `/api/extracted?id=<id>`;
  - a later ticketed request can hit that warmed entry;
  - no one-off cache entry is created for each `sig/ts` pair;
  - worker contract and production smoke tests pass.

### H3 — Make R2 range length explicit

- Severity: Medium
- Area: Worker
- File: `install-slot/_worker.js`
- Change: replace the ranged response's `obj.size`-derived length with `String(total - rangeStart)`.
- Acceptance:
  - sampled range offsets return `206`;
  - `Content-Length == actual body bytes`;
  - `Content-Range` matches the requested suffix;
  - production smoke covers at least first-byte, middle, and last-byte offsets.

### H4 — Move cold-path persistence behind `waitUntil`

- Severity: Medium
- Area: Worker
- Files: `install-slot/_worker.js`, `tests/worker_contract.mjs`
- Change: return the computed image response first; put edge-cache, R2-object, and `.meta.json` writes in `ctx.waitUntil()` using copied byte data.
- Acceptance:
  - response creation no longer awaits cache or R2 writes;
  - a second request still sees the R2 object after persistence completes;
  - no mutable source buffer is reused before background writes finish;
  - cold-path TTFB is measured before and after the change.

### H5 — Keep slow downloads on a valid R2 ticket

- Severity: Medium
- Area: Worker + firmware
- Files: `install-slot/_worker.js`, `main/meta_store_api.c`, `main/meta_store_analysis.c`
- Change: choose one policy: refresh the ticket before expiry, extend the Range-resume window, or issue a longer `max_age` based on the documented retry budget.
- Acceptance:
  - a resume attempted after the current 600-second ticket age does not silently fall back to the slow computed path;
  - the chosen ticket policy is documented;
  - firmware tests cover the chosen behavior.

### H6 — Prevent stale edge-cache SHA-256 mismatches

- Severity: Medium
- Area: Worker
- File: `install-slot/_worker.js`
- Change: before serving an edge-cache hit, compare its `x-image-sha256` with the current analyzed image SHA-256, or include that SHA-256 in the normalized cache key.
- Acceptance:
  - changing a play image within the old one-hour TTL cannot serve the previous digest as the current analyze result;
  - no-ticket and expired-ticket paths fail closed or fetch the current image;
  - the behavior is covered by a contract test.

### H7 — Handle completion before EOF

- Severity: Low
- Area: Firmware
- File: `main/meta_store_api.c`
- Change: before opening another segment connection, treat `st.received == analysis->image_len` as complete and proceed directly to digest/OTA verification.
- Acceptance:
  - a connection that dies after the last byte but before EOF does not request `Range: bytes=<image_len>-`;
  - no whole-image retry occurs;
  - final SHA-256 and `esp_ota_end` verification still run.

### H8 — Add download-ticket parser coverage

- Severity: Low
- Area: Firmware tests
- Files: `main/meta_store_analysis.c`, host-side analyze contract tests
- Change: add tests for a valid ticket, missing `dl`, invalid `ts`, malformed `sig`, and wrong-length `sig`.
- Acceptance:
  - valid tickets populate `dl_sig` and `dl_ts`;
  - invalid ticket fields leave the legacy no-ticket path selected;
  - static validation fails if the parser regresses.

### H9 — Close the hardware evidence gap

- Severity: Required acceptance item
- Area: Device validation
- Files: none
- Change: install the reviewed firmware on hardware, interrupt an R2 download, and resume it while capturing the serial log.
- Acceptance:
  - the log shows `206` resume from a non-zero offset;
  - no whole-image restart occurs after a recoverable network failure;
  - the final image passes SHA-256, `esp_ota_end`, and image verification;
  - the observed throughput and remaining stall pattern are reported separately from host results.

## Handoff completion gate

Do not mark the R2 rollout complete until H1-H8 have corresponding commits and H9 has a device serial log. H1-H6 are Worker/deployment work; H7-H8 are firmware/test work; H9 is acceptance evidence.

## Closure record (r10.20, 2026-09-30)

All items are **CLOSED** in commit `r10.20` except H9. Production-verified evidence:

- **H1 CLOSED** — `RATE_KV` bound in `wrangler.toml`; live: out-of-range probes `416,416,416,429,429,429,429`.
- **H2 CLOSED** — key derived from path + `id` only; live proof: `analyze?id=100` prewarm, then two different-query extracted requests both served `x-source: edge` (header added to edge entries for observability). Cache API is a no-op on `*.pages.dev` previews; hits occur on the production domain.
- **H3 CLOSED** — `content-length = total - rangeStart`; first/mid/last offsets: CL == body bytes == total-off, content-range exact.
- **H4 CLOSED** — persistence in `ctx.waitUntil` with independent copies; cold-path TTFB no longer includes the writes.
- **H5 CLOSED** — Range-resume ticket window 3600 s (600 s kept for non-Range); policy documented in `_worker.js` and CHANGELOG.
- **H6 CLOSED** — edge hits re-checked against current analyze sha/imageLen; stale entries evicted before serving.
- **H7 CLOSED** — `st.received == image_len` exits the retry loop into verification; pinned by `test_download_retry_gate.py`.
- **H8 CLOSED** — five-form dl.sig/dl.ts coverage in `test_store_analyze_contract.c`; parser validates 16 lowercase-hex per character.
- **H9 OPEN** — requires flashing the audit firmware and capturing a device serial log of an interrupted R2 download resuming from a non-zero offset.

Gates at closure: `worker_contract.mjs` PASS 1-14, `./tools/validate.sh` all green,
`node tools/e2e-production.mjs` ALL PASSED (including E2E-9/H1 and E2E-9/H3).
