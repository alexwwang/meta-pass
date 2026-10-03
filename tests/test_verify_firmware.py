#!/usr/bin/env python3
"""Host tests for the protected firmware-layout parser (dynslot safe table)."""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "verify_firmware", ROOT / "tools" / "verify_firmware.py"
)
assert SPEC and SPEC.loader
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)

# dynslot 安全表(partitions.csv),条目 offset 升序。
SAFE_ENTRIES = (
    (1, 2, 0x9000, 0x6000, "nvs"),
    (1, 1, 0xF000, 0x1000, "phy_init"),
    (0, 0, 0x10000, 0x170000, "factory"),
    (1, 0x40, 0x180000, 0x1D6000, "pool_0"),
    (1, 2, 0x356000, 0x4000, "cardid"),
    (1, 2, 0x35A000, 0x6000, "store"),
    (1, 0x40, 0x360000, 0x49E000, "pool_1"),
    (1, 0, 0x7FE000, 0x2000, "otadata"),
)

# 升级保留区(§4.6):nvs/store/otadata + 两池 —— 产物里必须全 0xFF。
EXPECTED_PRESERVED = {
    ("nvs", 0x9000, 0x6000),
    ("store", 0x35A000, 0x6000),
    ("otadata", 0x7FE000, 0x2000),
    ("pool_0", 0x180000, 0x1D6000),
    ("pool_1", 0x360000, 0x49E000),
}


def build_table(entries) -> bytes:
    raw = bytearray(b"\xff" * VERIFY.PARTITION_TABLE_SIZE)
    for index, (kind, subtype, offset, size, label) in enumerate(entries):
        VERIFY.ENTRY.pack_into(
            raw,
            index * VERIFY.ENTRY.size,
            0x50AA,
            kind,
            subtype,
            offset,
            size,
            label.encode().ljust(16, b"\0"),
            0,
        )
    marker = len(entries) * VERIFY.ENTRY.size
    struct.pack_into("<H", raw, marker, 0xEBEB)
    raw[marker + 16 : marker + 32] = hashlib.md5(raw[:marker]).digest()
    return bytes(raw)


def sample_table() -> bytes:
    return build_table(SAFE_ENTRIES)


class PartitionParserTest(unittest.TestCase):
    def test_parses_safe_layout_and_md5(self) -> None:
        partitions, found_md5 = VERIFY.parse_partition_table(sample_table())
        self.assertTrue(found_md5)
        by_label = {p.label: p for p in partitions}
        self.assertEqual(by_label["factory"].offset, 0x10000)
        self.assertEqual(by_label["factory"].size, 0x170000)
        self.assertEqual(by_label["cardid"].offset, VERIFY.CARDID_OFFSET)
        self.assertEqual(by_label["cardid"].size, VERIFY.CARDID_SIZE)
        self.assertEqual(by_label["store"].offset, 0x35A000)
        self.assertEqual(by_label["store"].size, 0x6000)
        self.assertEqual(by_label["pool_0"].size, 0x1D6000)
        self.assertEqual(by_label["pool_1"].offset, 0x360000)
        self.assertEqual(by_label["pool_1"].size, 0x49E000)
        self.assertEqual(by_label["otadata"].offset, 0x7FE000)

    def test_rejects_bad_md5(self) -> None:
        raw = bytearray(sample_table())
        raw[28] ^= 1
        with self.assertRaisesRegex(ValueError, "MD5"):
            VERIFY.parse_partition_table(bytes(raw))

    def test_committed_fixture_matches_in_code_table(self) -> None:
        # 仓库黄金产物(gen_esp32part.py 生成)与本测试构造的安全表逐字节一致。
        fixture = (ROOT / "tests" / "fixtures" / "safe_table.bin").read_bytes()
        self.assertEqual(fixture, sample_table())


class ProtectedLayoutTest(unittest.TestCase):
    def _merged(self, table: bytes) -> bytes:
        merged = bytearray(b"\xff" * VERIFY.FLASH_SIZE)
        merged[
            VERIFY.PARTITION_TABLE_OFFSET :
            VERIFY.PARTITION_TABLE_OFFSET + VERIFY.PARTITION_TABLE_SIZE
        ] = table
        merged[0x10000] = 0xE9
        return bytes(merged)

    def _build_dir(self) -> tempfile.TemporaryDirectory:
        directory = tempfile.TemporaryDirectory()
        (Path(directory.name) / "FoloToy-AI-Passport.bin").write_bytes(b"\xe9")
        return directory

    def test_layout_verification_accepts_safe_table(self) -> None:
        with self._build_dir() as directory:
            VERIFY.verify_protected_layout(
                self._merged(sample_table()), Path(directory)
            )

    def test_rejects_carved_table_in_artifact(self) -> None:
        # 发布产物只能携带安全表:出现任何 ota_* app 条目即拒(carved 表
        # 绝不能进产物 —— 槽位布局是设备运行时的 carve 决策)。
        carved = (0, 0x10, 0x180000, 0x1D6000, "ota_0")
        entries = tuple(
            e for e in SAFE_ENTRIES if e[4] != "pool_0"
        )
        entries = tuple(
            sorted(entries + (carved,), key=lambda e: e[2])
        )
        with self._build_dir() as directory:
            with self.assertRaisesRegex(ValueError, "app"):
                VERIFY.verify_protected_layout(
                    self._merged(build_table(entries)), Path(directory)
                )

    def test_rejects_missing_store_partition(self) -> None:
        entries = tuple(e for e in SAFE_ENTRIES if e[4] != "store")
        with self._build_dir() as directory:
            with self.assertRaisesRegex(ValueError, "store"):
                VERIFY.verify_protected_layout(
                    self._merged(build_table(entries)), Path(directory)
                )

    def test_preserved_regions_contract(self) -> None:
        self.assertEqual(set(VERIFY.UPGRADE_PRESERVED_REGIONS), EXPECTED_PRESERVED)

    def test_upgrade_safety_accepts_erased_regions(self) -> None:
        VERIFY.verify_upgrade_safety(self._merged(sample_table()))

    def test_upgrade_safety_rejects_dirty_pool(self) -> None:
        merged = bytearray(self._merged(sample_table()))
        merged[0x360000 + 0x1234] = 0x00
        with self.assertRaisesRegex(ValueError, "pool_1"):
            VERIFY.verify_upgrade_safety(bytes(merged))

    def test_upgrade_safety_rejects_dirty_store(self) -> None:
        # store 内容(carve 记录/Wi-Fi 备份)属于设备用户数据,产物不得携带。
        merged = bytearray(self._merged(sample_table()))
        merged[0x35A000] = 0x5A
        with self.assertRaisesRegex(ValueError, "store"):
            VERIFY.verify_upgrade_safety(bytes(merged))


if __name__ == "__main__":
    unittest.main()
