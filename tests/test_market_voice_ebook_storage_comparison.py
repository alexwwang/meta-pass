#!/usr/bin/env python3
"""Contract for source-based comparison of voice and e-book DATA storage."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/assets/mobile-page-data-e2e-market-comparison.zh_CN.md"
TARGET = ROOT / "docs/assets/mobile-page-data-e2e-market-firmware.zh_CN.md"

doc = DOC.read_text(encoding="utf-8")
target = TARGET.read_text(encoding="utf-8")

for token, label in [
    ("v2.5.0_folotoy-ai-passport.zip", "identify the public AI Passport firmware release asset"),
    ("partitions/v2/8m.csv", "pin the AI Passport partition layout"),
    ("`assets`", "identify the AI asset partition"),
    ("nvs_open", "ground settings persistence in source"),
    ("esp_partition_mmap", "ground asset access in source"),
    ("`userdata`", "identify the e-reader user-data partition"),
    ("esp_vfs_fat_spiflash_mount_rw_wl", "ground e-reader FAT access in source"),
    ("不能声称已完成固件二进制解包", "disclose that binary-level validation is incomplete"),
    ("Play 28 仍是当前最佳真实市场 DATA 验收目标", "preserve the evidence-based acceptance target"),
]:
    assert token in doc, f"missing voice/e-book storage evidence: {label}"

assert "mobile-page-data-e2e-market-comparison.zh_CN.md" in target
print("market voice/e-book storage comparison contract: PASS")
