#!/usr/bin/env python3
"""Contract for the selected real-market DATA E2E target and its explicit limits."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/assets/mobile-page-data-e2e-market-firmware.zh_CN.md"
FIXTURE_DOC = ROOT / "docs/assets/mobile-page-data-e2e-test-firmware.zh_CN.md"
PAGE_DOC = ROOT / "docs/mobile-page-e2e.zh_CN.md"

doc = DOC.read_text(encoding="utf-8")
fixture = FIXTURE_DOC.read_text(encoding="utf-8")
page = PAGE_DOC.read_text(encoding="utf-8")

for token, label in [
    ("Play 28", "select real recording firmware"),
    ("`recordings`", "use the actual runtime-generated recording partition"),
    ("`0x81`", "pin FAT partition subtype"),
    ("esp_vfs_fat_spiflash_mount_rw_wl", "ground selection in observed production mount path"),
    ("导出 WAV", "require file-level user-data evidence"),
    ("4 MiB", "document the proposed reduced carve size"),
    ("当前安装链路仍按固件声明的 DATA size", "do not imply size reduction already works"),
    ("BLOCKED / NOT RUN", "keep real business E2E blocked until the target path exists"),
]:
    assert token in doc, f"missing market DATA target contract: {label}"

assert "不是市场业务 DATA 验收固件" in fixture, "fixture must not be confused with market firmware"
assert "fixture PASS 不等于市场业务 E2E PASS" in page, "top-level E2E docs must preserve the acceptance boundary"
print("market DATA firmware selection contract: PASS")
