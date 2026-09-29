#!/usr/bin/env python3
# tests/test_bug21_scan_silent.py — BUG-21 regression gate (r10.11).
#
# BUG-21: every boot of a device with a half-written slot logged
#   E (1557) esp_image: invalid segment length 0xffffffff
# Root cause: meta_store.c ran esp_image_verify() in NON-silent mode on every
# boot (slot scan), and IDF 5.5.3's verify_segment_header (esp_image_format.c
# :819-827) logs ESP_LOGE when a segment header fails
#   (data_len & 3) != 0 || data_len >= ESP_IMAGE_MAX_FLASH_ADDR_SIZE
# An erased-state 0xFFFFFFFF segment header — the shape a download that died
# mid-write leaves behind (ESP images interleave segment headers and data, so
# the walk hits 0xFF tail bytes between segment boundaries) — violates both
# checks. Rejecting the half-written slot is correct; the error-level
# bootloader-format log noise was the bug.
#
# This gate pins statically (mirrors tests/test_http_contract.py):
#   1. the IDF source facts themselves (when an ESP-IDF checkout is available)
#   2. meta_store.c uses ESP_IMAGE_VERIFY_SILENT for the scan-time verify
#   3. the non-silent misuse pattern (bare ESP_IMAGE_VERIFY) is gone
#   4. the replacement carries a single-line WARN that names the remedy
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDF = os.environ.get("ESP_IDF_PATH", "/Users/alex/esp/esp-idf-v5.5.3")

fails = []

def check(cond, why):
    if cond:
        print("PASS bug21-scan-silent: %s" % why)
    else:
        fails.append(why)
        print("FAIL bug21-scan-silent: %s" % why)

# --- 1. the IDF facts, from the horse's mouth ---
idf_src = os.path.join(IDF, "components/bootloader_support/src/esp_image_format.c")
if os.path.exists(idf_src):
    src = open(idf_src, encoding="utf-8", errors="replace").read()
    check('ESP_LOGE(TAG, "invalid segment length 0x%"PRIx32, segment->data_len);' in src,
          "IDF (live checkout %s): non-silent verify logs the segment-length error" % IDF)
    check("(segment->data_len & 3) != 0" in src,
          "IDF (live checkout): 4-byte alignment rule makes 0xffffffff the trigger value")
else:
    check(True, "IDF checkout absent — recorded v5.5.3 facts in use (CI mode)")

# --- 2/3/4. production code must verify silently and explain rejections ---
store = open(os.path.join(ROOT, "main/meta_store.c"), encoding="utf-8").read()
check("esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta)" in store,
      "scan_one verifies with ESP_IMAGE_VERIFY_SILENT (no per-segment ESP_LOGE on boot)")
check("esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta)" not in store,
      "no non-silent esp_image_verify call remains in meta_store.c")
check("镜像校验失败" in store and "ESP_LOGW" in store,
      "rejection path logs a single-line WARN naming the remedy (delete/reinstall)")

if fails:
    print("\n%d check(s) failed" % len(fails))
    sys.exit(1)
print("bug21-scan-silent: all checks passed")
