[Chinese](app-data-lifecycle-design.zh_CN.md)

# APP and DATA Lifecycle Design (feat/storage)

## 1. Goals and invariants

The first version prioritizes a predictable lifecycle, explicit data loss, and bounded implementation complexity.

1. Every APP installation receives a fresh dynamic APP carve.
2. Each APP installation owns its DATA set; writable DATA is not shared across APPs by default.
3. **Uninstalling an APP always deletes all DATA associated with that APP. There is no standalone DATA-delete action.**
4. Users who need to preserve data must back it up before uninstall and restore it after reinstall. A reinstall is a new install instance.
5. No orphan DATA records or post-uninstall reassociation are supported.
6. Physical slot numbers and Flash offsets are placement details, not durable identity.

## 2. Identity and association

- `play_id`: logical APP/game identity.
- `install_id`: unique identifier for each successful installation, preventing accidental reuse of old DATA after reinstall.
- `data_id`: logical DATA set identifier; one APP may own multiple DATA partitions.
- `label`: stable purpose name within an APP, such as `save` or `assets`.

Association should point to the installation instance. If the current persistent format only stores `play_id + label`, treat this as a migration limitation; never use slot/offset as a substitute identity. If ownership cannot be established reliably, do not guess which DATA belongs to which APP.

## 3. Uninstall flow

Before erasing anything, persist the target APP's play_id, offset, and size as a committed NVS deletion intent. Erase DATA bytes and the APP image header, then remove the APP and all associated DATA records in one A/B carve commit; extents return to the pool only after that commit succeeds. When the local management service starts, it checks for pending intents and resumes idempotently by matching play_id + offset + size, never by trusting a stale slot index. If a legacy APP has play_id=0 while any DATA records exist, uninstall is rejected because ownership cannot be established safely.

Uninstall is irreversible cascade deletion. The UI exposes one action: “Delete APP and all its data.” It must not offer standalone DATA deletion or an option to retain DATA.

Recommended state machine:

1. **PRECHECK**: reject concurrent install/restore sessions; enumerate the APP and all associated DATA; validate records and pool bounds.
2. **BACKUP NOTICE**: if real byte-level backup is unavailable, clearly state that data cannot currently be backed up. Metadata-only export is not a backup. Require explicit confirmation of permanent deletion.
3. **DELETE_INTENT**: persist and verify a transaction record containing target APP, DATA extents, transaction ID, and stage before destructive operations.
4. **ERASE DATA**: erase each associated DATA extent and verify it. Persist progress so reboot recovery can resume.
5. **ERASE APP HEADER**: erase the APP image header sector so boot-time scanning cannot resurrect the deleted APP.
6. **COMMIT**: remove APP and all its DATA records in a recoverable persistent commit, update the partition table/pool record, and verify.
7. **RECLAIM**: return extents to the pool only after no committed record references them.
8. **DONE**: clear the transaction record and report success. On failure, retain recovery state; never report success or reuse extents still referenced by records.

### Atomicity and crash recovery

Flash erase cannot be physically rolled back. “Atomic” means a recoverable logical commit, not that bytes remain recoverable during erase. The durable intent must precede destructive work, and recovery must be idempotent.

Never release extents before removing references and safely erasing old content; never drop APP metadata and then asynchronously clean DATA without a durable transaction; never report success after erase failure; never clear only metadata while leaving an APP image header that boot scanning can recognize.

## 4. DATA deletion and sizing

- No standalone DATA-delete UI.
- No “keep DATA” choice during APP uninstall.
- All DATA labels owned by an APP are included in the same uninstall transaction.
- Users may choose DATA size within the supported pool and size bounds during install/configuration.
- Expansion uses migration: allocate temporary extent, copy and verify, atomically switch references, then release the old extent.
- Shrink is disabled unless the specific DATA format supports a verified safe shrink migration.
- DATA count is bounded by metadata limits and pool capacity; cross-APP writable sharing is out of scope.

## 5. Backup and restore

A complete backup contains actual DATA bytes, not just offset/size/state/label metadata. It should include format version and checksums, logical APP identity, source `install_id`, firmware/data-format version, each DATA label/type/size/format, actual bytes, and per-object SHA-256.

The archive must not depend on source slot numbers or Flash offsets. Restore validates the archive, allocates fresh extents, writes and verifies every object, and only then commits DATA records. Failed restore releases uncommitted extents. The target APP and format compatibility must be explicit.

**Implementation boundary:** the current `backup-data.js` / `meta_backup.h` path must be treated as incomplete unless actual DATA bytes are included. Until byte-level backup is implemented, UI must warn that uninstall permanently deletes data and must not claim backup/restore is available.

## 6. Safety invariants

- Install, uninstall, restore, and capacity migration are mutually exclusive pool-layout transactions.
- Every DATA extent is inside the DATA pool and overlaps neither APP, other DATA, nor reserved regions.
- Referenced extents cannot be reclaimed; uncommitted extents cannot be exposed as valid DATA.
- Pool capacity is available only after all APP/DATA references are removed and the commit is complete.
- Slot/offset changes cannot alter logical APP/DATA association.
- Success UI is driven by a device-side committed result, not a local toast.

## 7. Acceptance criteria

1. No standalone DATA deletion, keep-DATA, or orphan-archive option in the UI.
2. Uninstall enumerates and removes all DATA labels associated with the APP.
3. On success, APP/DATA records are absent, extents are unreferenced, and free-space accounting matches released extents.
4. Failure is not reported as success; still-referenced extents are never reallocated.
5. Interrupted uninstall recovers idempotently after reboot; deleted APP is not resurrected by image scanning.
6. Other APPs and their DATA bytes/records remain unchanged.
7. Every new install receives a fresh carve; old data is restored only from a complete backup.
8. Until byte-level backup exists, UI warns of permanent data loss.
9. Host tests cover normal uninstall, multiple DATA labels, erase/commit failure, repeated recovery, and record/extent consistency. Real-device evidence is tracked separately.
