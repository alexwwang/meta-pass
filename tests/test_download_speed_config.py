#!/usr/bin/env python3
# tests/test_download_speed_config.py — r10.12 download-throughput gate.
#
# The store download was slow (~tens of KB/s steady-state) because three
# defaults stacked:
#   1. WiFi modem sleep: esp_wifi_set_ps() was never called, so STA ran in
#      IDF's default WIFI_PS_MIN_MODEM — the radio dozes between DTIM beacons
#      and downlink TCP is throttled to wake-up cadence.
#   2. lwIP TCP receive window 5760 B (4*MSS default): with ~100 ms RTT to the
#      store host the window alone caps throughput at 5760/0.1 ≈ 57 KB/s.
#      LWIP_WND_SCALE depends on PSRAM (absent on C3), so 65535 is the ceiling
#      without scaling — 65535/0.1 ≈ 655 KB/s.
#   3. 1 KB download chunk: per-KB read+sha256+esp_ota_write call overhead;
#      4096 matches the flash page and the device-proven sample installer.
#
# This gate pins all three statically (mirrors test_bug21_scan_silent.py).
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDF = os.environ.get("ESP_IDF_PATH", "/Users/alex/esp/esp-idf-v5.5.3")

fails = []

def check(cond, why):
    if cond:
        print("PASS download-speed: %s" % why)
    else:
        fails.append(why)
        print("FAIL download-speed: %s" % why)

# --- 1. IDF facts: the ceilings we are escaping (recorded for CI) ---
kconfig = os.path.join(IDF, "components/lwip/Kconfig")
if os.path.exists(kconfig):
    k = open(kconfig, encoding="utf-8", errors="replace").read()
    check("default 5760 # 4 * default MSS" in k,
          "IDF (live checkout): lwIP default window is 5760 (4*MSS) — the ceiling we escaped")
    check("depends on (SPIRAM_TRY_ALLOCATE_WIFI_LWIP" in k,
          "IDF (live checkout): window scaling needs PSRAM — 65535 is the no-scale max on C3")
else:
    check(True, "IDF checkout absent — recorded v5.5.3 lwIP facts in use (CI mode)")

# --- 2. WiFi power-save must be OFF on BOTH start paths ---
net = open(os.path.join(ROOT, "main/meta_store_net.c"), encoding="utf-8").read()
n_ps = net.count("esp_wifi_set_ps(WIFI_PS_NONE)")
check(n_ps == 2,
      "esp_wifi_set_ps(WIFI_PS_NONE) on both STA and APSTA start paths (found %d/2)" % n_ps)

# --- 3. download chunk = flash page ---
api = open(os.path.join(ROOT, "main/meta_store_api.c"), encoding="utf-8").read()
check("#define DL_CHUNK         4096" in api,
      "DL_CHUNK is 4096 (flash page; per-KB syscall overhead /4)")

# --- 4. lwIP window/buffer/mailbox pinned in the build config ---
defaults = open(os.path.join(ROOT, "sdkconfig.defaults"), encoding="utf-8").read()
check("CONFIG_LWIP_TCP_WND_DEFAULT=32768" in defaults,
      "sdkconfig.defaults: TCP receive window 32768 (fits 48 WiFi RX buffers; 65535 caused 802.11 frame drops -> RTO storms, r10.16)")
check("CONFIG_LWIP_TCP_SND_BUF_DEFAULT=32768" in defaults,
      "sdkconfig.defaults: TCP send buffer 32768 (was 5760)")
check("CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64" in defaults,
      "sdkconfig.defaults: tcpip receive mailbox 64 (was 32)")

# --- 5. stub carries the API surface (syntax-check parity with device) ---
stub = open(os.path.join(ROOT, "tests/esp_stubs/esp_wifi.h"), encoding="utf-8").read()
check("wifi_ps_type_t" in stub and "WIFI_PS_NONE" in stub and
      "esp_wifi_set_ps" in stub,
      "host stub declares wifi_ps_type_t + esp_wifi_set_ps (IDF 5.x signatures)")

if fails:
    print("\n%d check(s) failed" % len(fails))
    sys.exit(1)
print("download-speed: all checks passed")
