<p align="right">
  <a href="play563-appstore-download-reverse.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Play 563 AppStore Download Reverse Analysis

Date: 2026-09-30  
Scope: user-supplied v50 serial log, hosted play 563 (`https://ai-passport.folotoy.cn/plays/563/`), the device-local page `http://192.168.0.19/appStore?t=aa54e022`, and meta-pass `main/meta_store_api.c`.  
Boundary: review and reverse analysis only; no firmware or Worker code was changed.

## Executive conclusion

The remaining speed problem is primarily the **meta-pass network path**, not the ESP32-C3 download loop.

Play 563 is not marked as an official play in the catalog API (`source=community`, `isOfficial=false`); it is a community AppStore play hosted on the official domain. Its binary downloads directly from the same nginx host that serves its metadata. The server ignores `Range`, and the firmware performs one full `200` download, verifies the catalog SHA-256 while streaming, and writes flash directly.

Meta-pass instead uses a ticketed Cloudflare Worker path, extracts an application image, and serves bytes from edge cache/R2. That path now supports correct Range resume, but the observed host/device path is still much slower than the direct AppStore origin. The serial log's successful resume proves that r10.17-r10.19 solved the *correctness* problem; it did not solve the *path latency/stall* problem. r10.21 addresses the worst firmware-side zombie wait, but it cannot make a slow upstream path fast.

## Evidence collected

### Play 563 metadata

The public `/plays/563/` page renders a generic AppStore frontend payload, so the useful source of truth is the device-proxied catalog API:

- `id/projectId`: `563`
- `slug`: `ai-passport-9`
- `title`: `AI Passport AppStore` (Chinese title also present in catalog metadata)
- `source`: `community`
- `isOfficial`: `false`
- `author`: Chinese display name U+65B0 U+7A7A U+6C14
- `firmwareSize`: `3,219,456`
- `firmwareSha256`: `2956f77bb1db312b5796cf8982bceed66be564d4590441a9fa8abfddb3fbdf01`
- `downloadUrl`: `/api/download/community/ai-passport-9`
- `firmware.format`: `esp-merged-0x0`
- `downloads`: `1560`
- `publishedAt`: `2026-09-29T02:50:44+00:00`

### Hosted binary behavior

Probe:

```text
GET https://ai-passport.folotoy.cn/api/download/community/ai-passport-9
Range: bytes=0-1023
```

Observed response:

- `HTTP/2 200`, not `206`
- `Content-Length: 3219456`
- no `Accept-Ranges` header
- `Cache-Control: private, no-store`
- the body starts at byte 0
- full-body SHA-256 matches the catalog value

Conclusion: the hosted play 563 endpoint does not implement Range. It does not need resume for its observed fast path because the direct origin is fast enough.

### Host timing comparison

Same host, same test window:

| Path | Protocol | Size | TLS | TTFB | Total | Throughput |
|---|---:|---:|---:|---:|---:|---:|
| Play 563 direct | HTTP/2 | 3,219,456 B | 0.105s | 0.142s | 0.395s | 8.15 MB/s |
| Play 563 direct | HTTP/1.1 | 3,219,456 B | 0.091s | 0.130s | 0.397s | 8.12 MB/s |
| meta-pass play 675 | HTTP/2 | 2,664,256 B | 0.435-0.844s | 0.969-3.380s | 6.78-9.50s | 0.39-0.47 MB/s |
| meta-pass play 675 | HTTP/1.1 | 2,664,256 B | 0.464s | 0.969s | 2.67s | 1.00 MB/s |

The meta-pass response was `x-source: edge`, `accept-ranges: bytes`, `age: 893`, with the expected `x-image-len` and `x-image-sha256` headers. So this measurement did not hit the old cold-transform path; the remaining gap is still the Cloudflare/Worker network path and its TLS/TTFB behavior.

The DNS addresses on this workstation are local proxy addresses (`198.18.0.0/16`), so absolute route geography is not proven here. The timing difference is still directly observable.

## Device-local AppStore reverse analysis

The local page at `/appStore?t=aa54e022` is a thin UI served by the currently running AppStore play. The embedded strings in the play 563 binary contain the same title, routes, API shape, user agent, and upstream origin.

### Local API surface

The page uses these device-local routes:

- `GET /api/cats`
- `GET /api/list`
- `GET /api/one?slug=...`
- `GET /api/featured`
- `GET /api/status`
- `GET /api/installed`
- `POST /api/install` with form fields `slug` and `title`
- `POST /api/remove`

The token `t` is required for mutating/detail routes. `/api/status` returns:

```json
{"busy":false,"percent":0,"name":"","slug":"","err":"","hrev":0}
```

The browser polls `/api/status` once per second while installing and once per five seconds while idle. Progress rendering is only a percentage; the device does not expose bytes/s, reconnect count, or server stage to the browser.

### Catalog proxying

Embedded firmware strings show the upstream endpoints:

- `https://ai-passport.folotoy.cn/api/plays?multiDevice=false&q=...`
- `https://ai-passport.folotoy.cn/api/plays/%s`
- `https://ai-passport.folotoy.cn/api/plays/recommendations`
- base URL `https://ai-passport.folotoy.cn`

The device local API proxies catalog metadata to the browser, then downloads the selected `downloadUrl` itself.

## Play 563 firmware download implementation

The play does not publish source (`githubUrl` is empty), so this section is from binary strings plus RISC-V disassembly of the published image.

### HTTP behavior

Disassembly of the `store_ota` path shows:

1. Build the final URL as `https://ai-passport.folotoy.cn` + `downloadUrl`.
2. Configure `esp_http_client` with a **20,000 ms** timeout (`0x4e20`).
3. Open the connection.
4. Require status `200`.
5. Read the stream in **2,048-byte** chunks.
6. Update SHA-256 while streaming.
7. Write the received bytes directly to flash according to the embedded image layout.
8. Treat early EOF or a read error as a failed install.

The config does not send `Range`; the status check accepts the full-body `200` path only. The endpoint probe independently confirms that Range is ignored.

The `esp_http_client` receive buffer is not explicitly enlarged in the observed config. With ESP-IDF 5.5.3, an unset `buffer_size` falls back to `DEFAULT_HTTP_BUF_SIZE == 512` (`esp_http_client.h`). The application-level read buffer is 2,048 bytes. This is smaller than meta-pass's 4 KiB buffer/chunk, reinforcing that buffer size is not the main speed difference.

### Image handling and verification

The binary is an `esp-merged-0x0` image. Its embedded partition table at `0x8000` contains:

```text
nvs      0x009000   24K
phy_init 0x00f000    4K
factory  0x010000    3M
otadata  0x310000    8K
cardid   0x356000   16K
ota_0    0x360000    3M
store    0x660000   16K
easter   0x664000  388K
recovery 0x700000    1M
```

The downloader captures the merged-image header/table prefix while streaming, builds an install plan, writes selected flash ranges directly, compares the stream SHA-256 with catalog metadata, and retries partition-table writes up to three times. Failure strings include:

- `Download interrupted`
- `Incomplete firmware download`
- `Firmware verification failed (SHA-256 mismatch)`
- `Failed to write ota_0`
- `Failed to write the partition table`

There is no resume logic and no multi-connection retry loop. This is a simpler trust model: metadata and binary come from the same HTTPS origin, so the catalog SHA-256 authenticates the stream. Meta-pass uses per-response `x-image-sha256` because its analyze and extracted endpoints are separate Worker paths.

## Reference mini-program installer comparison

The open-source `SHLcy/ai-passport-miniapp-installer` recovery installer is not play 563, but it implements the same general pattern:

- direct HTTPS URL from the mini program
- `timeout_ms = 15000`
- HTTP status must be `200`
- `Content-Length` must equal the metadata size
- 4 KiB read buffer
- stream SHA-256 verification
- direct flash installation
- no Range and no resume

Source: `/tmp/ai-passport-miniapp-installer-src/recovery/main/wifi_install.c:210-298` (cloned at review time).

This confirms that the common fast path in the AppStore ecosystem is a simple one-shot direct download, not a multi-hop resume protocol.

## v50 serial-log interpretation

The supplied v50 play 675 log proves Range resume correctness:

- First connection reached `900,188 / 2,664,256` bytes (33.8%).
- It then hit the old 30-second read ladder and died with `ESP_ERR_TIMEOUT`.
- The next connection resumed from `900,188` and completed the remaining 66.2%.
- No full re-download occurred.

Computed from the summarized timestamps:

- First connection average to 900,188 B: about **2.7 kB/s** over ~334s.
- The worst observed zombie interval moved only ~36 KiB from t=225s to t=373s: about **0.24 kB/s**.
- Resume segment moved `1,764,068` bytes in ~132s: about **13.3 kB/s** average.
- End-to-end time was roughly **469s**: inside the 600s initial-request ticket window and far inside the 3600s Range-resume window.
- The new TLS reconnect cost was ~2.4s, so waiting ~150s for a dead connection was not rational.

The current tree already contains the firmware-side correction for the specific zombie behavior: r10.21 uses two consecutive 15s reads, then reconnects, and raises the connection budget to eight. The log was produced by the older 3x30s ladder, so do not use it to re-open the already-fixed timeout issue.

## What remains unreasonable in the meta-pass implementation

### 1. The hot path has too much network machinery

Current firmware path (`main/meta_store_api.c:415-443`):

1. `analyze` produces metadata and a 600s ticket.
2. `extracted?id&ts&sig` goes through Cloudflare Worker routing.
3. The Worker serves edge cache/R2 and emits custom length/SHA headers.
4. Firmware verifies exact `Content-Length` / `Content-Range` / `x-image-sha256` on every connection.

That machinery is justified for meta-pass's extraction and multi-slot safety requirements, but it is materially slower and more failure-prone than the AppStore's direct same-origin download. The fix should start with serving/path selection, not more ESP-side buffering.

### 2. Ticket fallback is safe but silent

Current Worker code has two windows: `DL_TICKET_MAX_AGE_S = 600` for the initial full-body request and `DL_RANGE_TICKET_MAX_AGE_S = 3600` for Range resume requests (`install-slot/_worker.js:137-141`, `297-300`). An expired or missing ticket is not an authorization failure; it falls back to the legacy computed/edge-cache path. The firmware reuses the same `ts/sig` on every segment (`main/meta_store_api.c:423-431`), so a long install keeps the longer Range window after the first partial connection.

The v50 run consumed ~469s, so it fit inside both the initial 600s window and the 3600s resume window. The remaining issue is observability: after ticket expiry, the install can silently lose the R2 fast path and become slow without a firmware-visible reason.

Actionable direction: preserve the dual TTL design and expose the fast-path/fallback reason in a response or debug header. Do not treat the 600s initial ticket as an install-wide hard deadline.

### 3. The endpoint lacks a fast-origin fallback

The firmware has Range-safe fallback from `206` to whole-file `200`, but no alternate-origin fallback. If Cloudflare stalls, all resumes continue against the same path. The play 563 evidence shows that a direct nginx origin can be an order of magnitude faster on the same host/network.

Actionable direction: define a measured fallback origin for pre-extracted images, or bypass the Worker for immutable content where the trust model permits it. This is an architecture change and needs an explicit design decision before implementation.

### 4. Firmware telemetry still cannot explain *where* time went

The serial log can identify slow reads and reconnects, but it does not separate:

- DNS
- TCP connect
- TLS handshake
- request send
- TTFB
- body throughput

The host probes show TLS/TTFB differences are significant. Without per-stage timestamps, the next slow device log will still require guesswork.

Actionable direction: add one-line per-connection timing telemetry around open/fetch/read completion. Do not add persistent counters or a UI feature unless needed.

### 5. Buffer tuning is not the next lever

Meta-pass already uses a 4 KiB HTTP receive buffer and 4 KiB read chunks (`main/meta_store_api.c:433-443`, `541-553`). Play 563 uses a 2 KiB application read chunk and the IDF default 512-byte internal receive buffer, yet its direct path is much faster. The mini-program reference uses 4 KiB and a 15s timeout.

Conclusion: increasing ESP buffers again is unlikely to address the measured path difference.

## Recommended next experiments

Do these before code changes:

1. **Same-device A/B timing**: record one play 563 install and one meta-pass play 675 install back-to-back on the same Wi-Fi. Capture timestamped open, first byte, every 5% progress, reconnect, and total time.
2. **Host matrix**: run HTTP/1.1 and HTTP/2 curl timings for:
   - play 563 direct origin
   - meta-pass `analyze`
   - meta-pass ticketed full `extracted`
   - meta-pass `Range` at a large offset
3. **Ticket-expiry test**: start a device install, pause the network until the initial ticket is older than 600s, then resume. Expected current result: Range requests still use the 3600s resume window; after that window they fall back to the legacy path rather than failing authorization.
4. **Worker bypass prototype**: serve one immutable pre-extracted image from a direct origin or static CDN and measure the same device. If it matches play 563 throughput, the endpoint path is proven as the bottleneck.
5. **r10.21 hardware run**: repeat play 675 on the current build. The expected change is not higher peak throughput; it is losing at most ~30s per zombie connection instead of ~150s.

## Suggested implementation order, if changes are later approved

1. Expose ticket fast-path/fallback observability; keep the 600s initial / 3600s Range TTL split.
2. Add per-connection stage timing logs.
3. Choose and test a fast immutable binary origin / Worker bypass.
4. Only then consider client-side changes such as alternate origin fallback or more aggressive timeout values.

Do not remove Range resume to copy play 563. Play 563 can omit resume because its origin is fast and stable in the observed path; meta-pass's Cloudflare path has already demonstrated stalls where resume is necessary.

## Verification performed for this review

- Queried the device-local AppStore catalog and resolved play 563 to `ai-passport-9`.
- Downloaded play 563 from the hosted endpoint and verified the full-body SHA-256 against catalog metadata.
- Proved the hosted endpoint ignores Range and returns a full `200` body.
- Measured direct-origin and meta-pass endpoint timing on the same host.
- Disassembled the published play 563 app image and inspected the `store_ota` download path.
- Compared the behavior with the open-source mini-program recovery installer.
- Did not trigger a device install or modify local code.
