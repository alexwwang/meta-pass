#!/usr/bin/env python3
"""Static gate: the bootloader hook must enforce the dynslot carve BEFORE the
otadata single-session policy runs (design §4.4/§4.7, boundary B5).

A carved table that does not match committed store metadata has to be repaired
before otadata is honored, and a repair must wipe otadata so this boot lands on
factory. The enforcement lives in bootloader_components/meta_boot_hooks/hooks.c;
this gate pins the wiring that host tests cannot reach (flash side effects are
not linkable on the host). Same pattern as tests/test_bug21_scan_silent.py:
assert on source facts, with IDF checkout facts skipped when unavailable.

Checked contract:
  1. hooks.c includes the carve decision module and calls
     meta_carve_boot_decide() (the shared pure logic under test);
  2. the decide/enforce block appears BEFORE the otadata policy functions are
     called (enforce_single_session_on_copy / resume_running_slot_on_copy);
  3. a RESTORE_* verdict path erases otadata sectors and writes the partition
     table at ESP_PARTITION_TABLE_OFFSET (erase -> write -> re-read pattern);
  4. the hook component links the shared carve sources (CMakeLists SRCS).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOKS = ROOT / "bootloader_components" / "meta_boot_hooks" / "hooks.c"
CMAKE = ROOT / "bootloader_components" / "meta_boot_hooks" / "CMakeLists.txt"

errors: list[str] = []


def check(cond: bool, msg: str) -> None:
    if cond:
        print(f"ok: {msg}")
    else:
        errors.append(msg)


def main() -> int:
    src = HOOKS.read_text(encoding="utf-8")

    # 1. shared decision module wired in
    check('#include "meta_carve_boot.h"' in src,
          "hooks.c includes meta_carve_boot.h")
    check("meta_carve_boot_decide(" in src,
          "hooks.c calls meta_carve_boot_decide (shared pure logic)")

    # 2. enforcement before otadata policy: first decision call must precede
    #    the first single-session/resume call.
    decide_at = src.find("meta_carve_boot_decide(")
    policy_calls = [src.find(name)
                    for name in ("enforce_single_session_on_copy(",
                                 "resume_running_slot_on_copy(")]
    policy_calls = [pos for pos in policy_calls if pos != -1]
    check(decide_at != -1, "decide call present")
    check(bool(policy_calls) and all(decide_at < pos for pos in policy_calls),
          "carve decision runs before otadata single-session/resume policy")

    # 3. restore path: table write + otadata wipe (find the restore helper).
    restore_at = src.find("meta_carve_boot_restore")
    check(restore_at != -1, "hook defines a carve table restore path")
    if restore_at != -1:
        region = src[restore_at:]
        next_fn = region.find("\nstatic ", 1)
        region = region[:next_fn] if next_fn != -1 else region
        check("ESP_PARTITION_TABLE_OFFSET" in region,
              "restore writes the partition table at ESP_PARTITION_TABLE_OFFSET")
        check("bootloader_flash_erase_sector" in region,
              "restore erases sectors (table sector and/or otadata)")
        check("otadata" in region or "ota_offset" in region,
              "restore touches otadata (wipe -> boot factory)")
        check("bootloader_flash_read" in region and "memcmp" in region,
              "restore verifies by read-back comparison")

    # 4. component links the shared sources
    cmake = CMAKE.read_text(encoding="utf-8")
    check("meta_carve" in cmake,
          "hook component CMakeLists links shared meta_carve sources")
    check(re.search(r"meta_carve_boot\.c", cmake) is not None,
          "hook component compiles meta_carve_boot.c")

    if errors:
        for msg in errors:
            print(f"FAIL: {msg}", file=sys.stderr)
        return 1
    print("PASS test_dynslot_hook_gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
