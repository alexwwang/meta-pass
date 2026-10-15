<p align="right">
  <a href="mobile-page-backup-restore-e2e-handoff.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Mobile embedded-page backup/restore E2E handoff

Date: 2026-10-14
Branch: `feat/storage`

## Purpose

Close the remaining real-device acceptance gap for the mobile embedded page. USB backup/restore already has a complete real-device lifecycle report; that evidence does not prove the mobile UI path.

## Scope and safety

- Use the real device page in a mobile-sized Chromium/CDP session. Do not substitute the USB service page, mocked APIs, or passport-sim.
- Use a designated test device and a disposable test APP/DATA group only. Do not uninstall or overwrite personal apps or recordings. Stop if the baseline inventory contains data that has not been explicitly approved for destructive testing.
- Do not move or hardcode Flash addresses; do not modify the fixed `cardid` or `store` layout.
- Existing DATA in-place resize/migration, power-loss tests, and passport-sim (unless startup structure changes) are out of scope.
- Never include NVS raw bytes, session tokens, pairing codes, device IPs, or serial identifiers in committed logs.

## Required UI-driven sequence

1. Record baseline slot/DATA listing, free space, carve sequence, and device/firmware identity (redacted).
2. Open the actual mobile embedded page using CDP with a mobile viewport. Confirm no page/console/network errors.
3. Install one disposable test APP through the mobile page and choose a DATA size from the general selector.
4. Confirm APP/DATA ownership uses the same `play_id`; write known test bytes through the supported test-firmware control path and verify readback.
5. Trigger **Backup** from the mobile page and save the generated ZIP via its visible download link. Verify `manifest_version=2`, `complete=true`, exactly one APP group, and every payload length/SHA-256.
6. Trigger uninstall through the mobile UI and confirm the APP and its owned DATA are gone. Record the resulting free-space and carve state.
7. Load the saved ZIP through the mobile page's restore picker and trigger restore. Confirm success and a freshly allocated destination extent.
8. Independently read back the restored APP and DATA bytes through the supported device read path (or an independently controlled esptool dump when applicable); compare byte-for-byte/SHA-256 with the archive.
9. Reboot the device, reopen the mobile page, and repeat the ownership and byte checks. Verify destination NVS remains unchanged; archived NVS is integrity-checked but never written.
10. Delete only the disposable test APP through the mobile UI and verify the device returns to its recorded baseline geometry/free-space state.

## Acceptance

- The whole sequence is driven through the mobile page UI for install, backup, delete, and restore.
- No partial ZIP is produced after a backup failure.
- Restore accepts only a complete v2 single APP+DATA archive; malformed, incomplete, mismatched, or duplicate-ownership archives fail before mutation.
- APP/DATA bytes match the archive before and after reboot.
- Deletion removes only the test APP and its owned DATA; no orphan reservation remains.
- The report records commit SHA, commands, exit codes, screenshots, ZIP/manifest hashes, before/after listings, reboot evidence, and failure details. Redact secrets and do not commit the archive or NVS bytes.

## Not a substitute for

This handoff does not require a real market-firmware recording UI test, existing-DATA resizing, power-loss testing, or simulator testing. Keep each separately scoped.
