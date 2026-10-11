#!/usr/bin/env python3
"""Static safety contract for the dedicated DATA child firmware and serial client."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "tests/realdevice/data-child/main/test_main.c"
PARTITIONS = ROOT / "tests/realdevice/data-child/partitions.csv"
DRIVER = ROOT / "tools/realdevice/data_child_serial.py"
RUNTIME = ROOT / "tools/realdevice/mobile_page_runtime_driver.py"
CONTROL = ROOT / "main/meta_e2e_control.c"
CONFIG = ROOT / "main/Kconfig.projbuild"
DOC = ROOT / "docs/assets/mobile-page-data-e2e-test-firmware.zh_CN.md"

c = FIRMWARE.read_text(encoding="utf-8")
partitions = PARTITIONS.read_text(encoding="utf-8")
driver = DRIVER.read_text(encoding="utf-8")
runtime = RUNTIME.read_text(encoding="utf-8")
control = CONTROL.read_text(encoding="utf-8")
config = CONFIG.read_text(encoding="utf-8")
install_net = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
doc = DOC.read_text(encoding="utf-8")
record = (ROOT / "install-slot/dynslot-record.js").read_text(encoding="utf-8")
usb_page = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")

for token, label in [
    ("esp_partition_find_first", "DATA must be resolved by label through IDF partition API"),
    ('#define TAG_LABEL "e2edata"', "test DATA label is fixed and bounded"),
    ("esp_partition_erase_range(s_data, 0, 4096u)", "erase only the first DATA sector"),
    ("esp_partition_write(s_data, 0, &record, sizeof(record))", "write through resolved DATA partition"),
    ("esp_partition_read(s_data, 0, &verify, sizeof(verify))", "device-side readback"),
    ("record_valid(&verify)", "device-side digest and CRC validation"),
    ('strcmp(line, "REBOOT")', "explicit reboot command"),
    ('strcmp(line, "ERASE")', "explicit test-record erase command"),
]:
    assert token in c, f"missing firmware contract: {label}"

assert "e2edata,  data, 0x40" in partitions, "partition template must declare e2edata DATA"
assert "0x10000" in partitions, "DATA reservation must be 64 KiB in template"
extractor = (ROOT / "install-slot/extract-app-image.js").read_text(encoding="utf-8")
assert '(subtype === 0x40 && label === "e2edata")' in extractor, "only the explicitly named raw DATA E2E fixture may use subtype 0x40"
assert "other custom DATA is rejected" in extractor, "unregistered custom DATA subtypes must remain fail-closed"
assert "esp_partition_write(" not in driver, "host client must not write flash directly"
assert "static esp_err_t h_install_data_read(httpd_req_t *req)" in install_net, "mobile DATA backup needs a real byte-read endpoint"
assert "if (!origin_allowed(req))" in install_net and "if (!req_token_ok(req))" in install_net, "DATA read API must enforce origin and session token"
assert "length > INSTALL_IO_BUF" in install_net and "length > match->size - offset" in install_net, "DATA reads must be chunk-bounded and range-checked"
assert "esp_flash_read(NULL, read_buf, address, length)" in install_net, "DATA API must return actual Flash bytes, not metadata"
assert '{ "/api/install/data/read",HTTP_GET,  h_install_data_read,NULL }' in install_net, "DATA read handler must be registered"
assert "function bridgeBytes(path,opt)" in install_net and "dataRead:function(playId,label,offset,length)" in install_net, "phone bridge must expose binary DATA reads"
assert "expected_digest" in driver and "hashlib.sha256" in driver, "host independently checks digest"
assert "does not select/boot launcher slots" in driver, "serial client must not claim full runtime-driver coverage"
assert "CONFIG_META_E2E_TEST_CONTROL" in control and "default n" in config, "launcher control must be opt-in"
assert "meta_store_boot_slot(slot)" in control, "test control must reuse the validated production boot path"
assert "E2E BOOT_PLAY" in control and "expect_rejection" in runtime, "deleted play ID must be rejected by launcher boot policy"
assert "esp_partition_write" not in control and "esp_flash_write" not in control, "launcher control must not expose flash writes"
assert "carve.data.filter((record) => record.playId !== removed.playId)" in record, "APP removal must cascade to all DATA records owned by its nonzero playId"
remove_start = usb_page.index("async function removeSlotAction(index)")
remove_end = usb_page.index("// mock 连接", remove_start)
remove_action = usb_page[remove_start:remove_end]
assert remove_action.index("commitCarve(rm.carve") < remove_action.index("wipeRange(d.offset, d.size)"), "USB deletion must commit lifecycle metadata before erasing owned DATA bytes"
assert "slot.playId" in remove_action and "ownedData" in remove_action, "USB deletion must resolve DATA ownership by playId, not slot index"
assert "const dataImages = isFullImage(buf) ? extractDataImages(buf) : [];" in usb_page, "USB local firmware must parse DATA images instead of silently discarding them"
assert "await chooseUsbDataSizes(dataImages, carveForInstall)" in usb_page, "USB DATA installation must expose a capacity-selection step"
assert "await writeDataPayload(dataImage, dataImage)" in usb_page, "USB DATA payload must be written and read-back verified before commit"
assert 'data_backup_unavailable: "DATA byte-level backup/restore is not supported yet.' in usb_page, "USB UI must not imply DATA byte-level backup/restore exists"
assert "DATA partition bytes are not included" in usb_page, "APP slot archive must clearly exclude DATA bytes"
assert "不会包含 DATA 分区的实际字节" in usb_page, "Chinese APP archive description must disclose DATA omission"
assert "commitCarve(nextCarve, { materialize: deferredDataInstall })" in usb_page, "DATA-bearing APP install must commit APP+DATA as one carve/table transaction"
assert "boot-test-and-return" in runtime and "verify-deleted" in runtime, "runtime driver must match mobile-page runner contract"
assert "dataPersistedAfterReboot" in runtime and "deletedSlotNotBootable" in runtime, "runtime driver must emit required lifecycle evidence"
assert "USB Serial/JTAG" in doc and "尚未在 ESP-IDF 环境编译" in doc, "document hardware validation limitation"
print("DATA child firmware + serial protocol contract: PASS")
