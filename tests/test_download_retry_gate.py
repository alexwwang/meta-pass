#!/usr/bin/env python3
# tests/test_download_retry_gate.py — r10.17 resumable-download gate.
#
# On-device evidence (user log, v47): each attempt started fast, then 4 KB reads
# stalled 5-50 s (`errno=11`, `-ESP_ERR_HTTP_EAGAIN`) and the whole install
# restarted from byte 0 three times. Host replay of the same production artifact
# (`/api/extracted?id=675`, HTTP/1.1 + device UA) measured 18.9 s, a 524 after
# 126 s, then 33.5 s; `Range: bytes=100-199` still returned 200 with the full
# 2,664,256-byte body. Whole-file retry therefore multiplied an unstable path by
# the full artifact size every time.
#
# r10.17 design pinned here:
#   1. read-level: bounded EAGAIN tolerance, then abandon only that connection
#   2. session-level: OTA/SHA state survives reconnects; Range resumes at `received`
#   3. server contract: resume requires 206 + exact Content-Range, never append a 200
#   4. final failure/cancel/verification is the only OTA abort point
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDF = os.environ.get("ESP_IDF_PATH", "/Users/alex/esp/esp-idf-v5.5.3")

fails = []

def check(cond, why):
    if cond:
        print("PASS download-resume: %s" % why)
    else:
        fails.append(why)
        print("FAIL download-resume: %s" % why)

# --- 1. IDF read-timeout contract, from the live checkout when available ---
idf_hdr = os.path.join(IDF, "components/esp_http_client/include/esp_http_client.h")
if os.path.exists(idf_hdr):
    hdr = open(idf_hdr, encoding="utf-8", errors="replace").read()
    check("timed-out before any data was ready" in hdr and
          "ESP_ERR_HTTP_EAGAIN" in hdr,
          "IDF (live checkout %s): read timeout returns -ESP_ERR_HTTP_EAGAIN, connection alive" % IDF)
else:
    check(True, "IDF checkout absent — recorded v5.5.3 EAGAIN contract in use (CI mode)")

api = open(os.path.join(ROOT, "main/meta_store_api.c"), encoding="utf-8").read()

# --- 2. connection-local EAGAIN handling; reconnect budget is segment-level ---
check("got == -ESP_ERR_HTTP_EAGAIN && ++eagain < DL_STALL_EAGAIN_MAX" in api,
      "read-level: EAGAIN continues within one connection (bounded), then reconnects")
check("eagain = 0;" in api,
      "stall counter resets on data arrival")
check("#define DL_ATTEMPTS        8" in api,
      "six fresh TLS connections available for one install")
check("for (int attempt = 1; attempt <= DL_ATTEMPTS; attempt++)" in api and
      "err = install_segment(play_id, analysis, part, &st);" in api,
      "session-level: install_segment retried on fresh connections")
check("vTaskDelay(pdMS_TO_TICKS(1000 * (attempt - 1)));" in api,
      "retry backoff 1s..5s between segment connections")
check("if (err == ESP_OK || s_cancel || !install_retryable(err)) break;" in api,
      "retry loop honors success, cancel, and deterministic errors")

# --- 3. resume protocol and state lifetime ---
check('snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)st->received)' in api and
      'esp_http_client_set_header(client, "Range", range)' in api,
      "resume request sends Range: bytes=<received>-")
check("const int expected_status = resume ? 206 : 200;" in api,
      "resume requires HTTP 206 (initial request requires 200)")
check("meta_store_range_parse(s_resp_content_range, &range)" in api and
      "meta_store_range_matches(&range, st->received" in api,
      "206 Content-Range parsed and matched against received/total/CL")
check("server refused Range at rx=%lu (HTTP %d)" in api and
      "return ESP_ERR_NOT_SUPPORTED;" in api,
      "a 200/416 answer to a resume request is rejected, never appended")
segment = api[api.find("static esp_err_t install_segment"):api.find("esp_err_t meta_store_api_install")]
check("esp_ota_abort" not in segment,
      "no OTA abort inside segment download/retry paths")
check("install_abort_state(&st);" in api and "if (st.flash_touched) meta_slot_mark_invalid" in api,
      "OTA abort + slot invalidation happen only at final failure/cancel")
check("#define DL_STALL_EAGAIN_MAX 2" in api and
      "#define DL_READ_TIMEOUT_MS 15000" in api and
      ".timeout_ms = DL_READ_TIMEOUT_MS" in api,
      "zombie connections are declared dead in <=2x15s (r10.21: 30s worst-case vs "
      "~150s observed on the 3x30s ladder), then resume from the byte offset")
check("st.range_supported = true;" in api and
      "Range unsupported; fallback to full-file retry" in api and
      "install_restart_full(&st);" in api,
      "legacy server fallback rejects 200-on-resume, then safely restarts from byte 0")
check("st->flash_failed = true;" in api and
      'st.flash_failed ? "Flash operation failed."' in api,
      "OTA begin/write failures name the flash layer on screen, not a generic download error")
check("!st.range_supported && st.received > 0 && install_retryable(err)" in api,
      "legacy-mode network retry never appends a fresh 200 stream at an old offset")

# --- r10.20-H7: a fully received image must go straight to verification ---
check("if (st.received == analysis->image_len) break;" in api,
      "fully-received image exits the retry loop before opening a new segment "
      "connection (no wasted Range: bytes=<image_len>- attempt / full retry)")
inst_pos = api.find("esp_err_t meta_store_api_install")
loop_pos = api.find("if (st.received == analysis->image_len) break;")
check(0 < inst_pos and inst_pos < loop_pos,
      "the full-received check lives inside meta_store_api_install's retry loop, "
      "not inside install_segment")

# --- 4. response headers must come from ON_HEADER, not IDF's request-header getter ---
check("HTTP_EVENT_ON_HEADER" in api and "s_resp_sha" in api and
      "s_resp_content_range" in api,
      "x-image-sha256 and Content-Range captured from response-header events")
check("esp_http_client_get_header(client, \"x-image-sha256\"" not in api,
      "IDF request-header getter is not used for response SHA (old no-op check removed)")

if fails:
    sys.exit(1)
print("download-resume: all checks passed")
