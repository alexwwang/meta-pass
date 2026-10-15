#!/usr/bin/env python3
"""Static safety contract for device-side storage snapshot reads and bounded restore I/O."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
HEADER = (ROOT / "tests/esp_stubs/esp_partition.h").read_text(encoding="utf-8")
VALIDATE = (ROOT / "tools/validate.sh").read_text(encoding="utf-8")


def require(needle: str, label: str) -> None:
    assert needle in SOURCE, f"{label}: missing {needle!r}"


require('"/api/install/storage/info"', "registered partition inventory endpoint")
require("meta_pass_version", "inventory exposes the current MetaPass system version for archive compatibility checks")
require('"/api/install/storage/read"', "registered partition read endpoint")
require("esp_ota_get_running_partition()", "running host partition protection")
require('strcmp(label, "cardid") == 0', "cardid read protection")
require("install_carve_data_match(", "carve DATA identity fallback (launcher table omits DATA partitions)")
require("esp_flash_read(NULL, read_buf, part_offset + offset, length)", "carve DATA reads stay identity-bound and partition-relative")
require("length > INSTALL_IO_BUF", "bounded read length")
require("esp_partition_read(part, offset, read_buf, length)", "partition-relative read")
require("origin_allowed(req)", "same-origin gate")
require("req_token_ok(req)", "session token gate")
require("esp_partition_find(", "live partition table enumeration")
require("install_partition_same(part, running)", "running host identity comparison")
require('"/api/install/storage/erase"', "registered partition-scoped snapshot erase endpoint")
require('"/api/install/storage/write"', "registered partition-scoped snapshot write endpoint")
require("install_snapshot_target(req, &target", "restore uses the exact identity resolver")
require("esp_partition_erase_range(target.part, 0, target.size)", "erase is confined to one resolved partition")
require("esp_flash_erase_region(NULL, target.abs_offset, target.size)", "carve DATA erase is confined to the resolved record extent")
require("esp_partition_write(target.part, offset, write_buf, length)", "writes are partition-relative")
require("esp_flash_write(NULL, write_buf, target.abs_offset + offset, length)", "carve DATA writes stay identity-bound and partition-relative")
require("offset > part_size || length > part_size - offset", "write range stays inside partition bounds")
require("part->readonly", "read-only partition protection")
require("req->content_len != (size_t)length", "request body must match declared write length")
require("hcfg.max_uri_handlers = 24", "HTTP server has capacity for restore endpoints")
assert "ESP_PARTITION_TYPE_ANY" in HEADER and "esp_partition_iterator_t" in HEADER
assert "tests/test_storage_snapshot_device_contract.py" in VALIDATE
print("Storage snapshot device-read contract: PASS")
