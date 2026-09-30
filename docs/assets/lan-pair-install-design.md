English | [简体中文](lan-pair-install-design.zh_CN.md)

# LAN Phone-Assisted Install — Design (r10.22 proposal)

> Status: **design, not yet implemented**
> Date: 2026-09-30
> Trigger: on-device OTA over the C3's WAN path measured 5.7 kB/s average with 30 s
> zombie stalls (v50 log), while the same WiFi reachs the domestic market origin at
> multi-MB/s. The WAN hop, not the radio, is the bottleneck.

---

## 0. Goal and Non-Goals

**Goal**: a phone scan (later: typed IP) opens a preloaded page that renders the
market catalog locally, lets the user pick a play, downloads the firmware **on the
phone**, splits it locally if needed, and pushes it to the device over the **LAN** —
bypassing the device's slow WAN path entirely.

**Non-Goals**:

- Replacing the on-device store UI (it stays; this is an additional fast lane).
- Any new backend service. The existing Cloudflare Worker keeps serving the catalog
  and bytes; we only add static assets and one device-side endpoint.
- Changing the trust chain. The device remains the final authority
  (`esp_ota_end` + SHA-256 against analyze).

## 1. Why the Browser Forces This Topology (verified constraints)

| # | Constraint | Verified fact | Consequence |
|---|---|---|---|
| C1 | **Mixed content** | an `https://` page is forbidden from fetching `http://192.168.x.x`; device-side TLS is unrealistic on C3 | the landing page **must be opened from an http origin** — only the device can host it (`http://<device-ip>/install`) |
| C2 | **CORS: our Worker** | `access-control-allow-origin: *` on `/api/plays`, `/api/play`, analyze, extracted | phone JS may read catalog + bytes cross-origin ✓ |
| C3 | **CORS: market origin** | folotoy sends only `access-control-allow-credentials: true`, no allow-origin | phone JS **cannot** read market bytes directly; catalog/bytes must come from our Worker proxy (already exists) |
| C4 | **https → http script** | loading `https://metapass…/loader.js` into an `http://` page is legal | the "one JS downloaded from metapass" requirement survives: device serves a tiny HTML, the real logic loads from our Worker |

## 2. Architecture

```
device screen:  "http://192.168.x.x/install"  (QR later)
   │
phone: device-hosted landing page (http origin — C1)
   ├─ <script src="https://metapass.chuanxilu.net/install-loader.js">   (C4)
   ├─ catalog via Worker proxy (CORS *, C3)  → local rendering
   ├─ pick play → /api/analyze  → { extracted.sha256, imageLen, slots, dl ticket }
   ├─ fetch 2.66 MB extracted over phone WiFi/cellular (269-678 kB/s measured)
   │    (unpacking module exists and is tiny — 10.7 KB pure ESM — kept as the
   │     fallback path if we later switch to pulling the official 2.73 MB container)
   └─ POST http://<device-ip>/ota/install  (same-origin; sha256 + slot + pair code)
        └─ esp_http_server streams to esp_ota_write → esp_ota_end → boot policy
```

The last hop is LAN-only: 2.66 MB transfers in seconds. The slow WAN path is
not used for the image at all; the Worker's role shrinks to metadata + backup
byte source (the on-device OTA lane stays as the fallback path).

## 3. Components

### 3.1 Device (`main/`)

- `install_page` — static HTML (gzip'd, ~1-2 KB) served at `/install` by the
  existing `esp_http_server` (already in the build for the config portal).
- `/ota/install` POST endpoint:
  - headers: `X-Meta-Slot`, `X-Meta-Len`, `X-Meta-Sha256`, `X-Meta-Pair`
  - streaming: chunked reader → `esp_ota_write` per chunk (same pattern as the
    download loop; no full-image buffering)
  - header checks **before** `esp_ota_begin` (no flash erase on a rejected request):
    pair code mismatch → 403; slot unfit/unknown → 400; len > slot fit → 413
  - after stream: streaming SHA-256 vs `X-Meta-Sha256`, then `esp_ota_end`,
    then set boot slot via the existing slot/boot-policy code
- Pair code: 6-digit, shown on the device screen while the install page is
  active; any `/ota/install` without a match is 403'd. Mitigates "any LAN guest
  can write flash".

### 3.2 Worker (`install-slot/`)

- `/install-loader.js` — the whole phone app (catalog render, analyze, download,
  progress, push). Static asset, no backend change.
- CORS already open (C2). Nothing else changes; tickets/analyze stay as-is.

### 3.3 Phone page flow

1. probe `GET /hello` on the device origin (shows "phone not on the same WiFi"
   error otherwise — the one real UX failure mode)
2. catalog (Worker) → local render → user picks a play
3. analyze (Worker) → slot fit check locally, disable unfit slots
4. download extracted (Worker; `Range` resume applies)
5. push to device with progress; device verifies and reboots into the slot

## 4. Security Model

- **Flash-write authorization**: pair code is mandatory (403 otherwise). The
  pairing window exists only while the device shows it.
- **Byte integrity**: unchanged and device-authoritative. The phone cannot feed
  arbitrary bytes: SHA-256 must match analyze, `esp_ota_end` re-verifies the
  image, and the boot policy re-checks the slot signature chain.
- **No new secrets**: none on the phone; the pair code is transient and LAN-local.

## 5. Risks / Open Questions

| Risk | Mitigation / Status |
|---|---|
| Phone on cellular opens the QR | page probes the device first, explicit error + retry hint (r1) |
| iOS Safari + LAN http | legal from an http origin page; keep the landing http-origin **permanently** (do not "upgrade" it to https — that breaks C1 forever) |
| httpd RAM with 2.7 MB POSTs | chunked streaming, same as the download loop; measure during bring-up |
| Concurrent config-portal + install endpoints | same server instance, distinct URIs; no conflict expected, verify |
| LVGL QR widget not in build | v1 shows the IP on screen; QR is a follow-up |

## 6. Implementation Order

1. device: `/hello` + `/ota/install` + pair code (+ host tests for the gate logic)
2. device: `/install` landing page
3. worker: `install-loader.js` (catalog/analyze/download/push)
4. gates: contract tests for headers/errors, E2E from a phone on the LAN
5. docs: BUGS/CHANGELOG entries land with the code

## 7. Success Criteria

- 2.66 MB play installed from "scan to rebooted" in **under 1 minute** on the
  reference network (vs ~8 minutes WAN path).
- Zero changes to the trust chain; on-device install lane untouched as fallback.
