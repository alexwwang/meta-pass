<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Isolated: FAP_SCREENSHOT_V1 serial screenshot attempt

This directory quarantines an unfinished attempt to add a serial screenshot
service to the firmware: the host sends `FAP_SCREENSHOT_V1\n` over USB-CDC and
the device replies with a 240x320 RGB565 frame of the current LVGL screen
(protocol background:
[serial-screenshot-protocol.md](../../docs/reference/y2lin/serial-screenshot-protocol.md)).

## Why these files are isolated

- They are not part of any formal commit: both files are untracked, and this
  repository stages files explicitly (never `git add -A`).
- `main/CMakeLists.txt` uses an explicit `SRCS` list. An earlier change that
  listed `fap_screenshot.c` there (displacing `meta_md5.c`) was reverted, so
  the firmware build never compiles these files.
- No tracked source file includes `fap_screenshot.h`; a repo-wide search for
  `fap_screenshot` / `FAP_SCREENSHOT` under `main/`, `bootloader_components/`,
  `components/`, `tests/`, and `tools/` comes back empty.

## How to revive the experiment

1. Move `fap_screenshot.c` and `fap_screenshot.h` back into `main/`.
2. Add `"fap_screenshot.c"` to the `SRCS` list of `main/CMakeLists.txt`.
3. Start the service from `app_main` via `fap_screenshot_init()` /
   `fap_screenshot_task()`.

Until then, keep this directory out of every commit.
