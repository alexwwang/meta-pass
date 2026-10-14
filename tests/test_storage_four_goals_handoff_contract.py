#!/usr/bin/env python3
"""Keep the four-goal storage handoff tied to actual safety contracts and test entry points."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HANDOFF = (ROOT / "docs/assets/storage-four-goals-handoff.zh_CN.md").read_text(encoding="utf-8")
VALIDATE = (ROOT / "tools/validate.sh").read_text(encoding="utf-8")
PHONE = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
USB = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")
SERVER = (ROOT / "tools/install-slot/server.mjs").read_text(encoding="utf-8")
POOL = (ROOT / "install-slot/dynslot-pool.js").read_text(encoding="utf-8")
RESTORE = (ROOT / "install-slot/data-restore-plan.js").read_text(encoding="utf-8")
TRANSACTION = (ROOT / "install-slot/data-restore-transaction.js").read_text(encoding="utf-8")
CHILD = (ROOT / "tests/realdevice/data-child/main/test_main.c").read_text(encoding="utf-8")
DRIVER = (ROOT / "tools/realdevice/data_child_serial.py").read_text(encoding="utf-8")

# The handoff must enumerate the four requested outcomes and give the local agent
# reproducible commands, lifecycle assertions, and a reporting contract.
for token in (
    "手机内嵌页与 USB 服务页的 v2 快照路径已接入代码",
    "保留固定",
    "专用测试子固件验证 DATA 读写",
    "安装时创建 DATA 分区",
    "tools/realdevice/mobile_page_e2e.mjs",
    "tools/realdevice/usb_web_e2e.mjs",
    "重启前后清单与 SHA-256",
    "两页都接入快照备份与顺序恢复代码",
    "PASS / FAIL / BLOCKED / NOT RUN",
):
    assert token in HANDOFF, f"handoff missing required coverage: {token}"

# Explicitly guard the fixed partition geometry and prohibit claiming that
# address-free preflight alone constitutes a restore implementation.
assert "start: 0x180000, end: 0x356000" in POOL
assert "start: 0x360000, end: 0x7fe000" in POOL
assert "No Flash addresses or writes are accepted here" in RESTORE
assert "export async function readStorageSnapshot" in PHONE
assert "validateStorageSnapshotArchiveForDevice" in PHONE and "createStorageSnapshotArchive(bridge" in PHONE
assert "createStorageSnapshotArchive(bridge" in PHONE and "validateStorageSnapshotArchiveForDevice" in PHONE
assert "export async function restoreDataArchive" not in PHONE and "prepareDataArchiveRestore" not in PHONE
assert "tools/install-slot/test-data-archive-restore.mjs" in VALIDATE
assert "readStorageSnapshotZip" in PHONE
assert "buildStorageSnapshotRestorePlan" in PHONE
assert "tools/install-slot/test-data-restore-device-integration.mjs" in VALIDATE
assert "restore_data" in (ROOT / "main/meta_install_model.h").read_text(encoding="utf-8")
assert "m->restore_data ? META_DATA_DIRTY : META_DATA_PRISTINE" in (ROOT / "main/meta_install_model.c").read_text(encoding="utf-8")
assert 'from "./data-size-options.js"' in PHONE
assert 'from "./data-size-options.js"' in USB
assert "tools/install-slot/test-data-size-options.mjs" in VALIDATE
assert "tools/install-slot/test-data-restore-transaction.mjs" in VALIDATE
assert "executeDataRestoreTransaction" in TRANSACTION
assert "commitStarted" in TRANSACTION
assert "cleanupStaging" in TRANSACTION
assert "post-commit ownership/geometry verification failed" in TRANSACTION
assert "总空闲空间" in PHONE and "最大连续空闲空间" in PHONE
assert "total free" in USB and "max contiguous" in USB
assert "await restoreV2ArchiveUsb()" not in USB
assert "不再接受旧 APP+DATA v2 逻辑归档" in USB
assert "async function createUsbStorageSnapshot" in USB
assert "const BACKUP_CHUNK = 0x8000;" in USB
assert "const nvsData = await readFlashChunked(" in USB
assert 'from "./flash-readback-verify.js"' in USB
assert '"/flash-readback-verify.js"' in SERVER
assert "verifyFlashRangeByMd5" in USB
assert "loader.flashMd5sum(offset, length)" in USB
assert "md5-verify-done" in USB
assert "async function restoreV2ArchiveUsb()" not in USB
assert "async function createUsbStorageSnapshot" in USB
assert "USB 通过 ESP app descriptor 识别唯一 MetaPass 宿主分区" in HANDOFF
assert "async function restoreV2ArchiveUsb()" not in USB
assert "tools/install-slot/test-flash-readback-verify.mjs" in VALIDATE
assert "tools/install-slot/test-flash-read-transfer.mjs" not in VALIDATE
assert '"/flash-read-transfer.js"' not in SERVER
assert "恢复采用逐分区擦除与写入" in PHONE
assert "result.dataWritten" in PHONE and "请重启设备并确认 MetaPass 能正常启动" in PHONE
assert "result.dataWritten" in USB and "重启设备，确认 MetaPass 能正常启动" in USB
assert "data-written" in (ROOT / "install-slot/storage-snapshot-restore.js").read_text(encoding="utf-8")
assert "tests/test_storage_snapshot_restore.mjs" in VALIDATE

# USB-only local firmware installation remains a required asymmetric capability.
assert 'data-tab="local"' in USB and 'id="file-input"' in USB, "USB must retain its local-file firmware installation path"
assert "TAG_LABEL \"e2edata\"" in CHILD
assert "esp_partition_find_first" in CHILD
assert "REBOOT" in CHILD
assert "HELLO" in DRIVER and "INFO" in DRIVER

# The regression guard must run in the repository's standard static test suite.
assert "tests/test_storage_four_goals_handoff_contract.py" in VALIDATE
print("Storage four-goal handoff contract: PASS")
