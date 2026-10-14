# Handoff: USB segmented-session backup read

Language: [简体中文](HANDOFF-usb-backup-segmented-session.zh_CN.md)

## Goal

Validate the production USB backup button after replacing one long-lived flash-read session with bounded 512 KiB segments and a fresh Transport/ESPLoader pair between segments. A successful run must produce a complete archive from the page itself; do not use the host-assembled chunked archive as proof that this production path works.

## Preconditions

- Branch: `feat/storage`
- CI must be green for the tested HEAD.
- Use the Play 28 recorder fixture already documented in the Stage D report.
- Keep the device APP/DATA group intact during the first test. Do not delete or restore anything until the read-only backup is complete and independently verified.
- Run from the current checkout so the local server serves the tested `install-slot/install-slot.html`.

## Test sequence

1. Run `./tools/validate.sh` and `./tools/validate.sh --sim`; record both results and the exact HEAD.
2. Start the USB page server and the existing Chrome persistent profile that has permission for the device's Web Serial port.
3. Run `tools/realdevice/usb_data_backup_read_once.mjs` with a new evidence directory. This drives the real production `#btn-backup` path; the page should automatically read in 512 KiB segments, close/reopen the transport and loader between segments, and download one ZIP.
4. Verify the harness reports a downloaded archive, `manifest.complete === true`, exactly one APP group and its associated DATA reservation, and successful payload hash validation. Record total bytes, elapsed time, per-segment logs, and archive SHA-256.
5. Independently dump the APP image and DATA range with `esptool.py` in read-only mode. Compare byte-for-byte / SHA-256 against the corresponding archive payloads. Do not substitute device-side MD5 for reading and comparing the archive's actual bytes.
6. Reboot the device normally and confirm the original APP/DATA group remains visible and its metadata is unchanged. This is a read-only backup test, not a physical power-loss test.
7. If any segment restart fails, the page must report backup failure and must not download an incomplete archive. Preserve the logs and stop; do not proceed to destructive restore testing.

## Pass criteria

- One production UI action completes without manually reloading the page or running a host-side segment script.
- Every declared payload has the exact expected byte count and SHA-256.
- Independent `esptool.py` dumps match the APP and DATA payloads byte-for-byte.
- The APP/DATA ownership record is unchanged before and after backup and reboot.
- No ZIP is presented as successful when a segment or required DATA read fails.

## Failure evidence to return

Include the tested commit SHA, host/Chrome/OS versions, baud, archive manifest, archive SHA-256, APP/DATA SHA-256 values, segment/session restart logs, and the first failing offset/error. Redact device tokens, IPs, and serial identifiers. Do not run delete/restore tests until this read-only gate passes.
