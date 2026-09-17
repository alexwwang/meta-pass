<p align="right">
  <a href="backup-readflash-error-status-frame.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Round 6 of the backup-failure investigation — claimed "unconsumed stub error/status frame"

> Status: **RETRACTED (post-hoc correction, 2026-09-17 evening).** The claimed root cause
> below does not hold. Kept as an archive; see `debugging-workflow.md` for the actual
> final root-cause chain (unconsumed MD5 digest frame + baud-rate ceiling + stop-and-wait
> window), which is confirmed by on-device A/B measurement.

## The claim (as originally written)

The round-6 analysis claimed that the stub sends a standalone 2-byte error/status SLIP
frame after the 8-byte response header, that `checkCommand` leaves it in the stream, and
that the data loop in `readFlash` splices it into the payload — so every chunk would fail
the strict length check (`32770 > 32768`).

## Why it is wrong

- **Source re-check** (`stub_flasher.c`, esptool flasher_stub): the command loop emits
  `SLIP_send_frame_delimiter()` once, sends the 8-byte response header and the 2-byte
  error/status bytes, then the closing delimiter — header and status live in the
  **same SLIP frame**. `checkCommand` consumes the whole frame; nothing is left behind.
- **On-device disproof**: the probe button reads 4 KB and reports OK. If a +2-byte
  leftover frame existed, the strict length check would throw
  `Read more than expected: 4098 > 4096` on every read — it never appeared, and multiple
  full-slot backups succeeded the same day.

## What remains valuable from round 6

1. **chunkT0 scope bug (real)**: `const chunkT0` declared inside the retry `for` block but
   referenced by the debug success log outside it → `ReferenceError` whenever Debug mode
   was on and a chunk read succeeded. This fully explained "debug output missing + slot
   dies in ~3 s".
2. **Test-fidelity lesson (real, methodological)**: the unit-test mock stub did not mirror
   the real stub's frame sequence exactly, so unit tests stayed green while the device
   failed. Rule: a protocol mock must be derived from the server source, byte-for-byte,
   including frames the client currently ignores.

## Current final configuration (for context)

`block_size=4096, max_in_flight=64` (stop-and-wait, identical to esptool.py `read_flash`)
at 921600 baud. The 32768-byte pipelined window was reverted after A/B measurement showed
failure rate scaling with throughput (ACK uplink overlapping bulk data downlink on the
C3 USB-Serial-JTAG CDC endpoint), while esptool.py's stop-and-wait ran 5/5 clean.
