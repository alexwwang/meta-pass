#!/usr/bin/env python3
"""Static safety contract for device-side storage snapshot reads."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
HEADER = (ROOT / "tests/esp_stubs/esp_partition.h").read_text(encoding="utf-8")
VALIDATE = (ROOT / "tools/validate.sh").read_text(encoding="utf-8")


def require(needle: str, label: str) -> None:
    assert needle in SOURCE, f"{label}: missing {needle!r}"


require('"/api/install/storage/info"', "registered partition inventory endpoint")
require('"/api/install/storage/read"', "registered partition read endpoint")
require("esp_ota_get_running_partition()", "running host partition protection")
require('strcmp(part->label, "cardid") == 0', "cardid read protection")
require('part->address != part_offset || part->size != part_size', "exact live partition identity")
require("offset > part->size || length > part->size - offset", "relative range bounds")
require("length > INSTALL_IO_BUF", "bounded read length")
require("esp_partition_read(part, offset, read_buf, length)", "partition-relative read")
require("origin_allowed(req)", "same-origin gate")
require("req_token_ok(req)", "session token gate")
require("esp_partition_find(", "live partition table enumeration")
require("install_partition_same(part, running)", "running host identity comparison")
assert "/api/install/storage/write" not in SOURCE, "no arbitrary storage write endpoint may be introduced"
assert "/api/install/storage/erase" not in SOURCE, "no non-transactional storage erase endpoint may be introduced"
assert "ESP_PARTITION_TYPE_ANY" in HEADER and "esp_partition_iterator_t" in HEADER
assert "tests/test_storage_snapshot_device_contract.py" in VALIDATE
print("Storage snapshot device-read contract: PASS")
