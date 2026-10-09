#!/usr/bin/env python3
"""Static safety contract for the dedicated DATA child firmware and serial client."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "tests/realdevice/data-child/main/test_main.c"
PARTITIONS = ROOT / "tests/realdevice/data-child/partitions.csv"
DRIVER = ROOT / "tools/realdevice/data_child_serial.py"
DOC = ROOT / "docs/assets/mobile-page-data-e2e-test-firmware.zh_CN.md"

c = FIRMWARE.read_text(encoding="utf-8")
partitions = PARTITIONS.read_text(encoding="utf-8")
driver = DRIVER.read_text(encoding="utf-8")
doc = DOC.read_text(encoding="utf-8")

for token, label in [
    ("esp_partition_find_first", "DATA must be resolved by label through IDF partition API"),
    ('#define TAG_LABEL "e2edata"', "test DATA label is fixed and bounded"),
    ("esp_partition_erase_range(s_data, 0, 4096u)", "erase only the first DATA sector"),
    ("esp_partition_write(s_data, 0, &record, sizeof(record))", "write through resolved DATA partition"),
    ("esp_partition_read(s_data, 0, &verify, sizeof(verify))", "device-side readback"),
    ("record_valid(&verify)", "device-side digest and CRC validation"),
    ('strcmp(line, "REBOOT")', "explicit reboot command"),
    ('strcmp(line, "ERASE")', "explicit test-record erase command"),
]:
    assert token in c, f"missing firmware contract: {label}"

assert "e2edata,  data, 0x40" in partitions, "partition template must declare e2edata DATA"
assert "0x10000" in partitions, "DATA reservation must be 64 KiB in template"
assert "esp_partition_write(" not in driver, "host client must not write flash directly"
assert "expected_digest" in driver and "hashlib.sha256" in driver, "host independently checks digest"
assert "does not select/boot launcher slots" in driver, "serial client must not claim full runtime-driver coverage"
assert "USB Serial/JTAG" in doc and "not" in doc.lower(), "document control-path limitation"
print("DATA child firmware + serial protocol contract: PASS")
