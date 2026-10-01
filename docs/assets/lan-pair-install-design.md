English | [简体中文](lan-pair-install-design.zh_CN.md)

# LAN Phone-Assisted Install — Design

> Status: **device side implemented on `feat/mota` (2026-10-01): QR page, local HTTP
> install service, prepare → confirm → chunk → finalize UI; the phone web module
> and Worker are unchanged**  
> Branch: `feat/mota`  
> Date: 2026-09-30  
> Supersedes: the earlier r10.22 LAN-pair proposal in this file

## 0. Confirmed product decisions

This revision applies the decisions confirmed on 2026-09-30:

1. The phone web app performs market search, metadata display, analyze, firmware
   download, merged-image verification, app-image extraction, and LAN upload.
2. `metapass.chuanxilu.net` serves the phone app as dynamically loaded JavaScript.
   The launcher firmware does not embed market rendering or image-extraction logic.
3. Analyze is part of the phone-side install decision. The device has no separate
   analyze step. For v1, the device trusts the phone-supplied analysis manifest.
4. The current device-side numeric-ID market download flow is removed in
   `feat/mota`. The old code and behavior remain available from the previous
   branch/history for maintenance or rollback, but are not part of the new build.
5. v1 transport is LAN WiFi HTTP only. A future Bluetooth transport may reuse the
   same install-session/chunk state machine.

## 1. Goals and non-goals

### Goals

- Replace the on-device numeric play-ID flow with a phone-scanned install page.
- Let the phone browse and search the official market.
- Show the play name, firmware size, market revision, update time, and hashes.
- Let the phone decide installability during the install action.
- Download and split firmware on the phone.
- Push only the extracted app image to the selected meta-pass slot over LAN.
- Keep slot recommendation and physical user confirmation on the device.
- Keep the launcher small by moving market and extraction code out of firmware.

### Non-goals for v1

- No Bluetooth transport.
- No device-side official-market browsing UI.
- No device-side merged-image unpacking.
- No signed install manifest.
- No hidden device-side re-analyze.
- No auto-boot after install. The user returns to the launcher and boots the slot.

## 2. Browser-forced topology

The QR code must not point to an HTTPS page that then accesses a local HTTP
device. Browsers block that combination through mixed-content and private-network
rules.

The only robust v1 topology is:

```text
Device screen
  └─ QR: http://<device-lan-ip>/#s=<128-bit-session-token>

Phone browser
  ├─ opens the tiny device-hosted shell over local HTTP
  ├─ loads versioned JavaScript modules from https://metapass.chuanxilu.net
  ├─ calls metapass market/analyze/firmware APIs over HTTPS
  └─ calls the device over same-origin local HTTP
```

The token is in the URL fragment so it is not sent in the initial HTTP request.
The shell JavaScript reads it and sends it in a custom header for local API calls.

## 3. Trust model for v1

v1 intentionally uses a reduced trust model:

- The phone page obtains analyze data from metapass over HTTPS.
- The phone sends the resulting install manifest to the device over local HTTP.
- The device validates the manifest shape, slot fit, uploaded length, uploaded
  SHA-256, and ESP image structure.
- The device does not independently re-fetch analyze.
- Therefore the device cannot prove that the phone supplied the authentic
  metapass analysis.

The remaining protections are:

- one-time LAN session token;
- physical slot confirmation on the device;
- exact image length and phone-supplied hash binding;
- `esp_ota_end` and `esp_image_verify`;
- the slot remains INVALID on any failed write.

This is sufficient to prevent random LAN guests and corrupt transfers from
writing a slot. It does not protect against a compromised phone page that
deliberately swaps both the display metadata and the uploaded image. A signed
install manifest is the planned future hardening option.

## 4. Components

### 4.1 Device boot page and local proxy

The launcher starts a LAN install HTTP service after WiFi STA is online. The
device-hosted page is deliberately a **minimal boot page**, not the product UI:

- it owns the QR/session token;
- it loads the versioned remote loader;
- it exposes one small same-origin `DeviceBridge` for local status, prepare,
  session, chunk, finalize, and cancel requests;
- it receives user operations and firmware chunks from the loaded modules;
- it is the only page object that talks to the device write API.

The market UI, official-market metadata handling, analyze call, firmware
download, extraction, and progress rendering live in remotely loaded modules.
The boot page is the local proxy/bridge between those modules and the device.

```html
<script type="module"
        src="https://metapass.chuanxilu.net/phone-install/loader.v1.js">
</script>
```

The shell also shows a non-script fallback message with the device IP and the
current session state. The remote loader must be versioned and must pass a
protocol compatibility check before it may prepare an install.

### 4.2 Phone modules served by metapass

The Worker/static site serves these modules:

| Module | Responsibility |
|---|---|
| `loader.v1.js` | version handshake, dynamic imports, global error surface |
| `market.js` | official-market search, list, detail, metadata normalization |
| `install-core.js` | device session, confirmation polling, chunked upload |
| `extract-app-image.js` | existing merged-image validation and factory extraction |
| `name-blob.js` | display-name sanitizing and MNAM packing rules |
| `sha256.js` | pure-JS SHA-256; `crypto.subtle` is unavailable on local HTTP origins |

All modules are ordinary static assets with CORS enabled. They are not compiled
into the launcher.

### 4.3 Worker APIs

The existing metapass Worker remains the only WAN-side service.

Required behavior:

1. `GET /api/plays?...`
   - forwards the supported official-market query parameters, especially `q`;
   - returns CORS-enabled JSON;
   - does not cache market metadata.
2. `GET /api/play?id=<id>`
   - returns official play detail;
   - preserves `revisionId`, `updatedAt`, `firmwareSize`, `firmwareSha256`,
     `downloadUrl`, and the nested `firmware` object.
3. `GET /api/analyze?id=<id>`
   - existing contract;
   - returns `supported`, `reason`, `slots`, `suggestedSlot`,
     `extracted.imageLen`, and `extracted.sha256`.
4. `GET /api/firmware?path=<official-download-path>`
   - streams the official merged image instead of buffering the whole body in
     the Worker;
   - path whitelist remains fixed to `/api/download/`;
   - CORS enabled.
5. Static phone modules
   - versioned URL;
   - `access-control-allow-origin: *`;
   - immutable cache allowed only for content-addressed or versioned assets.

`/api/extracted` remains available for old launcher builds and as a diagnostic
fallback, but it is not the primary phone-install path.

## 5. Official-market metadata contract

The official detail API does not provide a semantic `version` field.

The phone UI displays:

- name: `title.zh` or `title.en`;
- store size: `firmware.size`, fallback `firmwareSize`;
- store SHA-256: `firmware.sha256`, fallback `firmwareSha256`;
- download path: `firmware.url`, fallback `downloadUrl`;
- market revision: `revisionId`;
- update time: `updatedAt`;
- extracted app version: parsed from the extracted ESP image when available.

Search uses the official `q` parameter through the Worker proxy. The phone may
additionally filter client-side, but remote `q` is the primary search path.

## 6. Install flow

### 6.1 Enter phone install mode

1. User selects the store/install entry on the launcher.
2. The launcher joins the saved WiFi network.
3. On success, it starts the LAN install HTTP service.
4. The screen shows:
   - QR code for `http://<device-ip>/#s=<token>`;
   - text fallback with IP and short pairing code;
   - timeout/cancel affordance.

If no WiFi credentials exist, the existing provisioning flow runs first.

### 6.2 Browse and choose on the phone

1. Phone opens the device shell.
2. Shell loads `loader.v1.js` from metapass.
3. Loader checks device/session protocol compatibility.
4. Phone searches the official market through the Worker.
5. User opens a play detail page and sees name, size, market revision, update
   time, and store SHA-256.

### 6.3 Install preflight on the phone

When the user taps install, the phone performs one integrated preflight:

1. `GET /api/analyze?id=<id>`.
2. If `supported=false`, show the reason and stop.
3. Download the merged image through `/api/firmware`.
4. Verify the merged image against the official `firmware.sha256`.
5. Extract the factory app with `extract-app-image.js`.
6. Compute the extracted app SHA-256 locally.
7. Compare local `imageLen` and SHA-256 with analyze.
8. Determine fit against the analyze slot table.
9. Send an install offer to the device.

There is no separate device-side analyze page or job.

### 6.4 Device confirmation

The install offer contains:

```json
{
  "protocol": 1,
  "playId": 563,
  "revisionId": 1499,
  "name": "ai-passport-9",
  "storeSha256": "<merged-image-sha256>",
  "imageLen": 1892032,
  "sha256": "<extracted-app-sha256>",
  "suggestedSlot": 0,
  "slots": [
    { "slot": 0, "limit": 1921024, "fit": true },
    { "slot": 1, "limit": 2093056, "fit": true },
    { "slot": 2, "limit": 2740224, "fit": true }
  ],
  "reason": "ok"
}
```

The device then:

1. validates the manifest shape and bounds;
2. displays the name, size, and warning/reason;
3. shows the three slots;
4. defaults to the offered `suggestedSlot` only if local partition geometry says
   it fits;
5. lets the user choose any locally fit slot;
6. requires physical confirmation;
7. marks the session ready for upload.

The phone cannot select or change the final slot without the device-side
confirmation state changing.

### 6.5 Chunked upload

v1 uses an install-session/chunk protocol instead of one monolithic POST.

#### Prepare

```text
POST /api/install/prepare
X-Meta-Session: <qr-token>
Content-Type: application/json
```

Body: the install offer above.

#### Session start

After the device-side slot confirmation:

```text
POST /api/install/session
X-Meta-Session: <qr-token>
Content-Type: application/json

{
  "imageLen": 1892032,
  "sha256": "<extracted-app-sha256>",
  "slot": 0
}
```

Response:

```json
{
  "state": "ready",
  "offset": 0,
  "maxChunk": 65536
}
```

The device rejects any mismatch with the confirmed offer.

#### Chunk write

```text
POST /api/install/chunk
X-Meta-Session: <qr-token>
X-Meta-Offset: 0
Content-Length: 65536
Content-Type: application/octet-stream
```

Rules:

- offsets must be exact and sequential;
- each chunk is no larger than `maxChunk`;
- the device splits large chunks internally into flash-write-sized pieces;
- duplicate already-written offsets are idempotent;
- gaps or rewinds are rejected;
- interruption leaves the session open until timeout or explicit cancel;
- retry starts at the device-reported offset.

#### Finalize

```text
POST /api/install/finalize
X-Meta-Session: <qr-token>
```

Device checks:

1. received byte count equals `imageLen`;
2. streaming SHA-256 equals the confirmed offer;
3. `esp_ota_end` succeeds;
4. `esp_image_verify` succeeds;
5. display-name blob is written;
6. slot registry becomes VALID.

Failure aborts the OTA write and marks the slot INVALID when flash was touched.

#### Status and cancel

```text
GET  /api/install/status
POST /api/install/cancel
```

Status exposes state, confirmed slot, offset, expected length, and short error
text. The device screen shows the same progress.

## 7. Firmware changes in `feat/mota`

> Implementation note (2026-10-01): the device-side cutover below has landed.
> New modules: `meta_store_install.{c,h}` (HTTP/OTA/token side effects) and
> `meta_install_model.{c,h}` (pure-logic manifest/session/chunk/finalize rules,
> host-tested via `tests/test_meta_install_model.c`). The old store-download
> modules (`meta_store_api/analysis/info_page/idedit/api_fail/range`), the WAN
> TLS/SNTP/certificate-bundle dependencies, and their tests/tools are removed.
> Verification: `tools/validate.sh --static` and `--firmware` both pass.

### Removed from the new launcher build

- numeric play-ID keypad page;
- device-side WAN download page;
- device-side download retry/cancel flow;
- `meta_store_api_install()`;
- Range-resume download logic;
- device-side market analysis parser/client;
- device store TLS/SNTP dependencies if no other launcher path uses them.

The old implementation remains in the previous branch/history. `feat/mota` is a
clean cutover, not a compatibility mode.

> Implementation note (2026-10-01, phone side): the v1 phone module MVP has landed as
> `install-slot/phone-install.js`, served by the Worker at `GET /phone-install.js`
> (CORS `*`, no-store) and loaded by the device boot page. The search/preflight/session
> flow follows §5/§6; the module ships its own pure-JS SHA-256 for plain-HTTP origins
> and is pinned by `tests/test_phone_install.mjs`. The `loader.v1.js` filename is
> reserved for a future versioned-loader split.

### Retained and reused

- WiFi credential storage and provisioning;
- slot registry and local fit checks;
- `esp_ota_begin/write/end`;
- streaming SHA-256;
- `esp_image_verify`;
- display-name/MNAM write;
- boot/delete/rollback policy;
- store session timeout discipline.

### New device components

- QR renderer and text fallback;
- LAN install HTTP service active in STA mode;
- install-session state machine;
- manifest parser with strict bounds;
- chunk/offset validator;
- token and pairing-code gate;
- local upload progress snapshot.

## 8. Security rules

- QR token: at least 128 bits of randomness.
- Short typed fallback: six digits, short TTL, attempt limit, single session.
- All mutating local endpoints require `X-Meta-Session`.
- No `Access-Control-Allow-Origin` on device install APIs.
- If an `Origin` header exists, it must match the device origin.
- Only one install session may exist.
- Upload is impossible before physical slot confirmation.
- `imageLen`, `sha256`, and `slot` in the upload session must exactly match the
  confirmed offer.
- The device never executes or trusts phone-supplied code.
- The phone page never receives a persistent device credential.

## 9. Performance expectations

The slow path removed from the device is:

```text
ESP32-C3 TLS -> Cloudflare Worker/R2 -> multi-MB body
```

The v1 path is:

```text
phone browser -> metapass/official-market proxy -> phone memory
phone browser -> LAN HTTP -> ESP32-C3 flash
```

Expected benefits:

- ESP32-C3 no longer performs the WAN bulk transfer.
- Phone browsers handle TLS, retransmission, and congestion better.
- Only the extracted app image crosses the LAN.
- Chunk resume avoids restarting a complete upload after a phone interruption.

Actual LAN throughput must be measured on the first hardware bring-up. The design
does not assume a specific LAN rate.

## 10. Future Bluetooth path

Bluetooth is not implemented in v1.

The session/chunk protocol is intentionally transport-neutral so a future BLE
transport can map:

- `prepare` -> GATT manifest write;
- `session` -> GATT session-open characteristic;
- `chunk` -> sequential GATT writes with offset acknowledgment;
- `finalize` -> GATT finalize command;
- `status` -> notification.

Browser constraints remain significant: Web Bluetooth is not a reliable iOS
Safari path. A native app may be required for broad phone support.

## 11. Acceptance criteria

### Market browsing

- Search by Chinese and English keywords returns official-market results.
- Detail page shows name, size, market revision, update time, and store SHA-256.
- A play without required firmware metadata is not installable.

### Phone extraction

- Store hash mismatch rejects the download.
- Non-ESP32-C3 image rejects.
- Missing factory partition rejects.
- Image larger than every slot rejects.
- Local extracted length/hash must match analyze.

### Device confirmation

- Phone cannot upload before device confirmation.
- Slot mismatch is rejected.
- Unfit slot cannot be selected.
- Occupied slot warning remains visible before confirmation.

### Upload

- Wrong token is rejected.
- Wrong offset is rejected.
- Oversized chunk is rejected.
- Interrupted upload resumes at the device-reported offset.
- Hash mismatch finalizes as failure and marks the slot INVALID.
- Successful install appears in the launcher with the selected name.

### Compatibility

- Android Chrome works.
- iOS Safari works.
- Phone on cellular without LAN access receives a clear same-WiFi error.
- Browser matrix includes the WeChat embedded browser before release.

## 12. Explicit v1 limitation

v1 trusts the phone-supplied analyze manifest. This is an accepted temporary
tradeoff to keep the firmware small and the flow simple. The next hardening step
is a metapass-signed install manifest verified by a device-embedded public key;
that step can remove the remaining metadata-spoofing risk without restoring a
device-side WAN analyze path.
