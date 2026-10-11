#!/usr/bin/env python3
"""Keep the four-goal storage handoff tied to actual safety contracts and test entry points."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HANDOFF = (ROOT / "docs/assets/storage-four-goals-handoff.zh_CN.md").read_text(encoding="utf-8")
VALIDATE = (ROOT / "tools/validate.sh").read_text(encoding="utf-8")
PHONE = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
USB = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")
POOL = (ROOT / "install-slot/dynslot-pool.js").read_text(encoding="utf-8")
RESTORE = (ROOT / "install-slot/data-restore-plan.js").read_text(encoding="utf-8")
CHILD = (ROOT / "tests/realdevice/data-child/main/test_main.c").read_text(encoding="utf-8")
DRIVER = (ROOT / "tools/realdevice/data_child_serial.py").read_text(encoding="utf-8")

# The handoff must enumerate the four requested outcomes and give the local agent
# reproducible commands, lifecycle assertions, and a reporting contract.
for token in (
    "手机内嵌页与 USB 服务页对齐",
    "保留固定",
    "专用测试子固件验证 DATA 读写",
    "安装时创建 DATA 分区",
    "tools/realdevice/mobile_page_e2e.mjs",
    "tools/realdevice/usb_web_e2e.mjs",
    "重启前后 SHA-256 对照",
    "PASS / FAIL / BLOCKED / NOT RUN",
):
    assert token in HANDOFF, f"handoff missing required coverage: {token}"

# Explicitly guard the fixed partition geometry and prohibit claiming that
# address-free preflight alone constitutes a restore implementation.
assert "start: 0x180000, end: 0x356000" in POOL
assert "start: 0x360000, end: 0x7fe000" in POOL
assert "No Flash addresses or writes are accepted here" in RESTORE
assert "当前版本尚不支持导入恢复" in PHONE
assert 'from "./data-size-options.js"' in PHONE
assert 'from "./data-size-options.js"' in USB
assert "tools/install-slot/test-data-size-options.mjs" in VALIDATE
assert "总空闲空间" in PHONE and "最大连续空闲空间" in PHONE
assert "total free" in USB and "max contiguous" in USB
assert "v2 restore before any device writes" in (ROOT / "tests/test_data_child_firmware_contract.py").read_text(encoding="utf-8")

# USB-only local firmware installation remains a required asymmetric capability.
assert 'data-tab="local"' in USB and 'id="file-input"' in USB, "USB must retain its local-file firmware installation path"
assert "TAG_LABEL \"e2edata\"" in CHILD
assert "esp_partition_find_first" in CHILD
assert "REBOOT" in CHILD
assert "HELLO" in DRIVER and "INFO" in DRIVER

# The regression guard must run in the repository's standard static test suite.
assert "tests/test_storage_four_goals_handoff_contract.py" in VALIDATE
print("Storage four-goal handoff contract: PASS")
