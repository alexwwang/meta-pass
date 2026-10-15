# DATA Byte-Level Backup and Restore Design

<p align="right">
  <strong>English</strong> · <a href="data-byte-backup-restore-design.zh_CN.md">简体中文</a>
</p>

Date: 2026-10-11  
Branch: `feat/storage`

## Goal and Current Status

Align DATA lifecycle semantics between the USB service page and the embedded mobile page: install DATA, back up its actual bytes, restore those bytes, and delete DATA owned by an APP when that APP is uninstalled.

**End-to-end backup/restore is not implemented yet.** USB dynslot v2 export includes actual bytes for all DATA reservations owned by selected APPs, including PRISTINE reservations, with per-file SHA-256 validation and a post-read A/B ownership-record consistency check. Legacy/v1 layouts remain APP/NVS-only. Mobile export now reads the full allocated APP slot and all owned DATA reservations in 4 KiB authenticated chunks, rechecks ownership metadata, validates every length/SHA-256, and emits a ZIP. DATA restore is not implemented: the USB UI explicitly rejects v2 archives before any device writes; only legacy v1 APP/NVS archives enter the existing restore path. A v2 archive must not be treated as restorable yet.

## Invariants

- Do not move `cardid` or `store`; do not change the fixed partition layout.
- Every DATA address must come from a validated carve record. Clients must not supply arbitrary Flash addresses.
- DATA ownership is keyed by `(play_id, label)`, never by a slot-array index.
- Uninstall ordering remains: commit deletion of APP and owned DATA metadata first, then erase bytes. If device recovery state is uncertain, reject concurrent writes.
- A metadata-only manifest must never be described as a data backup. Do not report success until byte readback verification passes.
- Do not run destructive automated tests against a device containing user data.

## Proposed Archive Format: MPTB v2 ZIP

Reuse the ZIP container and manifest but increment the manifest version. Version 1 continues to mean APP/NVS backup only; it must not be assumed to contain DATA.

```json
{
  "manifest_version": 2,
  "kind": "meta-pass-slot-backup",
  "created_utc": "ISO-8601",
  "slots": [
    {
      "play_id": 123,
      "source_slot": 1,
      "name": "example",
      "files": [{"name": "slot-1.bin", "bytes": 12345, "sha256": "...", "type": "app"}]
    }
  ],
  "data": [
    {
      "play_id": 123,
      "label": "cfg_save",
      "type": 1,
      "subtype": 64,
      "state": 1,
      "bytes": 65536,
      "sha256": "...",
      "file": "data/123/0.bin"
    }
  ]
}
```

The manifest must not store source Flash addresses that could be reused as write targets. Archive paths must be normalized safely; labels are logical identities and must not be used as unescaped paths.

Archives also carry an explicit `complete` flag. If any selected slot could not be read, a verified subset may still be exported, but the manifest must set `complete: false` and list `missing_slots`. The UI must label it as a partial backup, never as a complete one.

## Backup Flow

1. Read slots and DATA reservations; validate carve geometry and installer state.
2. Read each DATA reservation by `(play_id, label)` in chunks of at most 4 KiB.
3. Compute SHA-256 over the complete content and store actual bytes in the ZIP. The manifest records length, digest, label, type/subtype, and state.
4. Re-read reservation metadata and confirm identity, offset, and size did not change during export. Discard the archive if they changed.
5. Mark the archive complete only if APP files and all owned DATA are present and all length/digest checks pass. Partial output must be labelled partial, never a complete backup.

## Restore Flow

1. Parse and validate the entire archive before device writes: version, file presence, lengths, SHA-256, duplicate identities, paths, size granularity, and record limits.
2. Require every DATA group to match an APP with the same `play_id` in the archive. If that ID already exists on the target device, reject or require an explicit replace/uninstall policy; never silently overwrite live data.
3. Allocate fresh DATA extents with the shared allocator; never reuse source addresses from the archive. Check total capacity, contiguous space, record count, and partition-table capacity.
4. Write DATA to unregistered staging extents, then read it back and verify SHA-256. Stop and best-effort erase staged extents on mismatch.
5. Write and validate APP images, then publish APP and DATA ownership in one carve/table commit. Before metadata commit, failures may erase staging. If commit has started and its result is ambiguous, preserve staging bytes and enter recovery inspection rather than blindly erasing.
6. Re-read slots and DATA reservations, verify ownership, geometry, size, and non-overlap, then verify bytes again. Only then report success.

## Mobile API Contract

A read-only endpoint has been added:

- `GET /api/install/data/read`
- Requires a valid `X-Meta-Session` and passes the device's existing Origin validation.
- Headers: `X-Meta-Play-Id`, `X-Meta-Data-Label`, `X-Meta-Offset` (offset relative to the DATA partition), and `X-Meta-Length`.
- Read length is 1–4096 bytes per request. The server resolves the carve record by `(play_id, label)` and validates the range; requests cannot supply arbitrary Flash addresses.
- Success returns `application/octet-stream`; failures return non-2xx status.
- Mobile `DeviceBridge.dataRead(playId, label, offset, length)` returns a byte array and HTTP status.

Still required:

- Mobile DATA restore/import UI and the device-side write transaction.
- Restore endpoint with authorization, capacity preflight, staged writes, readback verification, and atomic metadata commit.
- USB and mobile v2 export now include real APP-slot/DATA bytes with validation; still required: v2 restore transaction, recovery semantics, and repeatable mock-device/hardware tests.
- Repeatable mock-device tests for the device API, USB ZIP, and mobile UI, plus dedicated hardware tests.

## Acceptance Matrix

| Scenario | Required result |
|---|---|
| DATA read range exceeds reservation | Reject; never return adjacent Flash bytes |
| Missing token, invalid Origin, unknown play ID, or label | Reject |
| Length is zero or exceeds 4 KiB | Reject |
| Archive file missing, wrong length, or wrong SHA-256 | Reject before restore writes |
| Total DATA capacity fits but contiguous space does not | Reject in preflight; no writes |
| DATA record limit exceeded | Reject in preflight; no writes |
| Staging write or readback fails | Do not commit ownership; best-effort cleanup only within known staging range |
| Metadata commit result is ambiguous | Preserve staging bytes, report uncertainty, and require recovery inspection |
| After restore and reboot | DATA digest matches and APP remains bootable |
| APP uninstall | Delete owned DATA records first, then erase bytes; preserve other APPs' DATA |
| Version 1 archive | Clearly disclose that it excludes DATA; never silently treat it as complete |

## Implementation Boundary

The current code provides a token/Origin-protected, ownership-keyed, 4 KiB-bounded mobile DATA read API, binary `DeviceBridge.dataRead`, shared MPTB v2 manifest/payload validation, and USB dynslot export of real DATA bytes with SHA-256 verification. Mobile APP+DATA archive export is implemented; the DATA restore/write transaction and repeatable end-to-end mock/hardware tests are still missing. Passing CI does not prove real-device byte-level backup or reboot persistence.
