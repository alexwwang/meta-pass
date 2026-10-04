#!/usr/bin/env python3
"""Golden fixture generator for the dynslot table codec tests.

Regenerates tests/fixtures/*.bin with ESP-IDF's gen_esp32part.py (the byte
authority for the partition-table format, MD5 marker included). The generated
files are committed so host tests run in bare CI checkouts without IDF; when
IDF is available this script can be re-run and `git diff` must stay empty.

Usage:  python3 tests/fixtures/gen_fixtures.py [path/to/gen_esp32part.py]

Sources:
  safe_table.bin            <- partitions.csv (dynslot safe table)
  legacy_table.bin          <- partitions.csv @ git HEAD 9e591a2..c3f340a
                               (the frozen v1.x 3-slot table; regenerated from
                               the literal CSV below, which is byte-identical
                               to git show HEAD:partitions.csv before feat/dynslot)
  carve_migration_table.bin <- safe fixed entries + legacy ota_0/ota_1/ota_2
                               slots at identical offsets (migration result,
                               design §4.6: installed plays untouched)
  carve_shrunk_table.bin    <- 2 custom slots: ota_0 shrunk to 0x80000 at
                               0x180000, ota_1 sized 0x200000 at 0x360000
  play563_table.bin         <- table bytes cut from the hosted play 563 merged
                               image (SHA-256 2956f77b…3fbdf01, verified
                               2026-10-02; NOT regenerated here)
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

LEGACY_CSV = """\
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x170000,
ota_0,    app,  ota_0,   0x180000, 0x1D6000,
cardid,   data, nvs,     0x356000, 0x4000,
ota_1,    app,  ota_1,   0x360000, 0x200000,
ota_2,    app,  ota_2,   0x560000, 0x29E000,
otadata,  data, ota,     0x7FE000, 0x2000,
"""

# gen_esp32part enforces ascending offsets, which is also the materializer's
# emit rule (entries merged by offset; otadata always last).
CARDID_STORE = """\
cardid,   data, nvs,     0x356000, 0x4000,
store,    data, nvs,     0x35A000, 0x6000,
"""
OTADATA = "otadata,  data, ota,     0x7FE000, 0x2000,\n"
HEAD = """\
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x170000,
"""

MIGRATION_CSV = (HEAD +
                  "ota_0,    app,  ota_0,   0x180000, 0x1D6000,\n" +
                  CARDID_STORE +
                  "ota_1,    app,  ota_1,   0x360000, 0x200000,\n"
                  "ota_2,    app,  ota_2,   0x560000, 0x29E000,\n" +
                  OTADATA)

SHRUNK_CSV = (HEAD +
               "ota_0,    app,  ota_0,   0x180000, 0x80000,\n" +
               CARDID_STORE +
               "ota_1,    app,  ota_1,   0x360000, 0x200000,\n" +
               OTADATA)


def default_tool() -> str:
    home = Path.home()
    for cand in (
        home / "esp/esp-idf-v5.5.3/components/partition_table/gen_esp32part.py",
        home / "esp/esp-idf/components/partition_table/gen_esp32part.py",
    ):
        if cand.is_file():
            return str(cand)
    raise SystemExit("gen_esp32part.py not found; pass its path as argv[1]")


def generate(tool: str) -> None:
    jobs = [
        (ROOT / "partitions.csv", HERE / "safe_table.bin"),
        (None, HERE / "legacy_table.bin"),          # LEGACY_CSV (temp file)
        (None, HERE / "carve_migration_table.bin"),  # MIGRATION_CSV
        (None, HERE / "carve_shrunk_table.bin"),     # SHRUNK_CSV
    ]
    csvs = {"legacy_table.bin": LEGACY_CSV,
            "carve_migration_table.bin": MIGRATION_CSV,
            "carve_shrunk_table.bin": SHRUNK_CSV}
    for src, dst in jobs:
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
            fh.write(csvs.get(dst.name, "") if src is None else src.read_text())
            tmp = fh.name
        subprocess.run([sys.executable, tool, "--flash-size", "8MB", tmp, str(dst)],
                       check=True, capture_output=True)
        print(f"generated {dst.name} ({dst.stat().st_size} B)")


if __name__ == "__main__":
    generate(sys.argv[1] if len(sys.argv) > 1 else default_tool())
