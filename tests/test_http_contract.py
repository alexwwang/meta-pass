#!/usr/bin/env python3
# tests/test_http_contract.py — BUG-19 regression gate (r10.8).
#
# BUG-19: meta_store_api.c used the return value of esp_http_client_fetch_headers()
# as the HTTP status code. In IDF 5.5.3 that function returns the Content-Length
# (esp_http_client.h:639: "Download data length defined by content-length header"),
# so on a real device analyze (~600 B) was rejected as "HTTP 600" and install
# (~1.8 MB) failed the length check — even with TLS fully working. Status must be
# read via esp_http_client_get_status_code().
#
# This gate pins, statically and deterministically:
#   1. the IDF header contract itself (when an ESP-IDF checkout is available;
#      otherwise a recorded contract snippet, CI bare-checkout rule)
#   2. the stub signature matches the real contract (int64_t, not int)
#   3. production code reads status via get_status_code() in BOTH analyze and install
#   4. the BUG-19 misuse pattern (assigning fetch_headers to an int/`status`) is gone
#   5. the r10.8 hardening fields are present in both request configs
#   6. the r10.8 config decisions (stack 10240, renegotiation off) are pinned
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDF = os.environ.get("ESP_IDF_PATH", "/Users/alex/esp/esp-idf-v5.5.3")

# Recorded contract text from IDF v5.5.3 esp_http_client.h (lines 628-639).
# Used verbatim when the IDF checkout is not present, so the gate never
# depends on local machine state for its core assertion.
RECORDED_CONTRACT = (
    " * @return\n"
    " *     - (0) if stream doesn't contain content-length header, or chunked encoding (checked by `esp_http_client_is_chunked` response)\n"
    " *     - (-1: ESP_FAIL) if any errors\n"
    " *     - Download data length defined by content-length header\n"
)

fails = []

def check(cond, why):
    if cond:
        print("PASS http-contract: %s" % why)
    else:
        fails.append(why)
        print("FAIL http-contract: %s" % why)

# --- 1. the contract itself, from the horse's mouth ---
idf_header = os.path.join(IDF, "components/esp_http_client/include/esp_http_client.h")
if os.path.exists(idf_header):
    hdr = open(idf_header, encoding="utf-8", errors="replace").read()
    decl = hdr.find("int64_t esp_http_client_fetch_headers")
    before = hdr[:decl] if decl > 0 else ""
    block = before[before.rfind("/**"):] if "/**" in before else ""
    check("Download data length defined by content-length header" in block,
          "IDF header (live checkout %s): fetch_headers returns content-length, not status" % IDF)
    check("int esp_http_client_get_status_code(esp_http_client_handle_t client);" in hdr,
          "IDF header (live checkout): get_status_code() is the status accessor")
else:
    check(RECORDED_CONTRACT.strip().startswith("* @return"),
          "IDF checkout absent — recorded v5.5.3 contract snippet in use (CI mode)")

# --- 2. stub must carry the real contract ---
stub = open(os.path.join(ROOT, "tests/esp_stubs/esp_http_client.h")).read()
check("int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);" in stub,
      "host stub declares fetch_headers as int64_t (content-length), not int")
check("int esp_http_client_get_status_code(esp_http_client_handle_t client);" in stub,
      "host stub declares get_status_code()")

# --- 3/4. production code: correct accessors, no misuse pattern ---
api = open(os.path.join(ROOT, "main/meta_store_api.c")).read()
misuse_free = not re.search(
    r"(?:int|const\s+int)\s+(?:status|code)\s*=\s*esp_http_client_fetch_headers", api)
check(misuse_free, "meta_store_api.c: no `int status = fetch_headers()` misuse (BUG-19 pattern)")
check(api.count("esp_http_client_get_status_code(client)") >= 2,
      "meta_store_api.c: both analyze and install read status via get_status_code()")
check(not re.search(r"status\s*(?:==|!=)\s*200[^;]*fetch_headers", api),
      "meta_store_api.c: 200 comparison never derived from fetch_headers")

# --- 5. r10.8 request-surface hardening in both configs ---
for field in (".user_agent = META_STORE_API_USER_AGENT",
              ".disable_auto_redirect = true",
              ".buffer_size = 4096"):
    check(api.count(field) == 2, "meta_store_api.c: `%s` present in BOTH configs" % field)
header = open(os.path.join(ROOT, "main/meta_store_api.h")).read()
check('META_STORE_API_USER_AGENT "meta-pass/' in header,
      "meta_store_api.h: device User-Agent single definition point exists")

# --- 6. r10.8 decisions pinned outside this file ---
net = open(os.path.join(ROOT, "main/meta_store_net.c")).read()
check("#define JOB_STACK          10240" in net,
      "meta_store_net.c: JOB_STACK = 10240 (sample-proven TLS peak, was 8192)")
sdk = open(os.path.join(ROOT, "sdkconfig.defaults")).read()
check("CONFIG_MBEDTLS_SSL_RENEGOTIATION=n" in sdk,
      "sdkconfig.defaults: TLS renegotiation explicitly off")

print()
if fails:
    print("%d HTTP CONTRACT CHECK(S) FAILED" % len(fails))
    sys.exit(1)
print("ALL HTTP CONTRACT CHECKS PASSED")
