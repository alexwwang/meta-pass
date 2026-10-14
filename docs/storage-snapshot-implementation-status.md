<p align="right">
  <strong>English</strong> · <a href="storage-snapshot-implementation-status.zh_CN.md">简体中文</a>
</p>

# Storage Snapshot Implementation Status (2026-10-14)

Branch: `feat/storage`.

## Verified on the current branch

- Latest checked HEAD: `95eea1911646eefcc5ab679317f7929195ca4601`.
- GitHub Actions **Static checks** and **Firmware checks** both passed for that HEAD.
- `install-slot/storage-snapshot.js` provides the v2 snapshot manifest, partition geometry/protection/extent validation, ZIP encode/decode, SHA-256 checks, sparse erased-range representation, and a non-mutating restore-plan builder.
- The device exposes authenticated, same-origin read-only partition inventory and partition-relative read APIs. Reads of the running host partition and `cardid` are rejected; callers cannot submit absolute Flash addresses as read targets.
- The embedded phone page supports v2 whole-device snapshot backup with a sensitive-data warning. Restore is deliberately preflight-only: it validates the archive and target layout but does not write Flash.
- The USB page retains the historical v1.1 fixed-slot archive path. The NVS restore write path has an explicit contract test; do not replace this legacy path with v2 snapshot parsing.
- A real-device report from an earlier tested revision records a successful segmented APP+DATA logical-archive backup/delete/restore cycle and intermittent Chrome Web Serial/esptool-js failures. That report is historical evidence only: the current branch has retired the old APP+DATA logical-v2 restore handler, so it does **not** prove current-HEAD restore behavior. Re-run the applicable USB v1.1 compatibility regression on the current firmware/page before hardware acceptance.
- The tests explicitly preserve the version boundary: USB historical v1 restore remains identifiable, the retired APP+DATA logical-v2 restore handler is absent, and the phone page accepts only the v2 whole-device snapshot discriminator.

## Not complete / blocked

- **USB v2 snapshot backup remains fail-closed.** The USB serial/bootloader path cannot yet obtain a trustworthy report of the currently running host partition. Do not infer it from a static partition table, hard-coded addresses, or guessed OTA state.
- **v2 snapshot restore is not enabled on either page.** The device has no safe transactional erase/write API with protected-range enforcement, recovery journal, disconnect/power-loss behavior, and post-write verification. Keep restore preflight-only; do not add arbitrary-address write/erase endpoints.
- The historical USB v1.1 archive path is a separate compatibility contract. Do not broaden v1 archive support to the phone page; phone remains v2-only.
- Old APP+DATA logical-v2 archives must be rejected as a distinct retired format, never treated as `meta-pass-storage-snapshot`.
- Real-device testing is external to GitHub CI. The existing report covers the USB segmented backup/delete/restore scenario for the old logical archive path; it does not prove that the new whole-device v2 snapshot can be backed up over USB or restored.

## Safety requirements

Before enabling any snapshot restore write, validate the entire archive, every payload's length and SHA-256, the target device's live partition layout, and protected ranges before mutation. The device must independently enforce partition identity, bounds, sector alignment, protected ranges, and transaction state. A restore plan must cover only explicitly eligible partitions and must never expose arbitrary Flash addresses. Never move `cardid` or bypass the dynamic allocator.
