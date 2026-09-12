#!/usr/bin/env python3
"""Host tests for the protected firmware-layout parser."""

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


def sample_table() -> bytes:
    entries = (
        (1, 2, 0x9000, 0x6000, "nvs"),
        (1, 1, 0xF000, 0x1000, "phy_init"),
        (0, 0, 0x10000, 0x170000, "factory"),
        (0, 0x10, 0x180000, 0x1D6000, "ota_0"),
        (1, 2, 0x356000, 0x4000, "cardid"),
        (0, 0x11, 0x360000, 0x200000, "ota_1"),
        (0, 0x12, 0x560000, 0x29E000, "ota_2"),
        (1, 1, 0x7FE000, 0x2000, "otadata"),
    )
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


class PartitionParserTest(unittest.TestCase):
    def test_parses_protected_layout_and_md5(self) -> None:
        partitions, found_md5 = VERIFY.parse_partition_table(sample_table())
        self.assertTrue(found_md5)
        by_label = {p.label: p for p in partitions}
        self.assertEqual(by_label["factory"].offset, 0x10000)
        self.assertEqual(by_label["factory"].size, 0x170000)
        self.assertEqual(by_label["cardid"].offset, VERIFY.CARDID_OFFSET)
        self.assertEqual(by_label["cardid"].size, VERIFY.CARDID_SIZE)
        self.assertEqual(by_label["ota_0"].offset, 0x180000)
        self.assertEqual(by_label["ota_1"].offset, 0x360000)
        self.assertEqual(by_label["ota_2"].offset, 0x560000)

    def test_rejects_bad_md5(self) -> None:
        raw = bytearray(sample_table())
        raw[28] ^= 1
        with self.assertRaisesRegex(ValueError, "MD5"):
            VERIFY.parse_partition_table(bytes(raw))


class ProtectedLayoutTest(unittest.TestCase):
    def test_layout_verification_accepts_current_partition_table(self) -> None:
        merged = bytearray(b"\xff" * VERIFY.FLASH_SIZE)
        merged[
            VERIFY.PARTITION_TABLE_OFFSET :
            VERIFY.PARTITION_TABLE_OFFSET + VERIFY.PARTITION_TABLE_SIZE
        ] = sample_table()
        merged[0x10000] = 0xE9

        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            (build_dir / "FoloToy-AI-Passport.bin").write_bytes(b"\xe9")
            VERIFY.verify_protected_layout(bytes(merged), build_dir)


if __name__ == "__main__":
    unittest.main()
