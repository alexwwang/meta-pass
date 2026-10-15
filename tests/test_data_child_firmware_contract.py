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
phone_page = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
smoke = (ROOT / "tools/realdevice/smoke.py").read_text(encoding="utf-8")
run_smoke = (ROOT / "tools/realdevice/run_smoke.py").read_text(encoding="utf-8")

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
assert "h_install_nvs_info" in install_net and "h_install_nvs_read" in install_net, "mobile NVS API handlers must exist"
reset_start = usb_page.index("const failedPort = newTransport?.device;")
reset_end = usb_page.index("if (transport === newTransport)", reset_start)
reset_handler = usb_page[reset_start:reset_end]
assert reset_handler.index("failedPort.setSignals") < reset_handler.index("newTransport?.disconnect()"), "failed handshake reset must pulse signals before disconnect closes the port"
assert "dataTerminalReady: true, requestToSend: false" in reset_handler, "reset pulse must release EN with the boot strap inactive"
assert "dataTerminalReady: false, requestToSend: false" in reset_handler, "reset pulse must return DTR to idle"
assert 'msg_reset_failed' in reset_handler, "automatic reset failure must be visible instead of silently swallowed"
assert 'self.fw.write(re.sub(r"(token=)[0-9a-f]{32}", r"\\1<redacted>", line, flags=re.I))' in smoke, "UART evidence must redact session tokens while retaining the in-memory pairing token"
assert 'ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs"' in install_net, "NVS API must resolve only the fixed system partition"
assert "length > INSTALL_IO_BUF" in install_net and "length > part->size - offset" in install_net, "NVS reads must be bounded and range checked"
assert '{ "/api/install/nvs/info", HTTP_GET, h_install_nvs_info, NULL }' in install_net and '{ "/api/install/nvs/read", HTTP_GET, h_install_nvs_read, NULL }' in install_net, "NVS endpoints must be registered"
for token in ('stage("S1b 系统 NVS 只读 API")', 'NVS info 拒绝未认证请求', 'NVS 首块读取失败', 'NVS 尾块读取失败', 'NVS 越界读取应返回 416', 'Cache-Control', '不记录 NVS 原始字节'):
    assert token in smoke, f"real-device smoke must verify NVS API safety: {token}"
assert "expected_digest" in driver and "hashlib.sha256" in driver, "host independently checks digest"
assert "does not select/boot launcher slots" in driver, "serial client must not claim full runtime-driver coverage"
assert 'r"=== (S\\d+[a-z]? .+?) ==="' in run_smoke, "real-device report parser must include S1b and future lettered stages"
assert '"tool_versions"' in run_smoke and '"run_id"' in run_smoke, "real-device report must capture run identity and tool versions"
assert '"NVS_API"' in run_smoke, "real-device report must classify NVS endpoint failures separately"
assert '{"report.json", "report.md"}' in run_smoke, "report artifact list must include the generated reports"
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

# Device uninstall must publish removal metadata before touching APP/DATA bytes.
remove_start = install_net.index("static esp_err_t remove_app_bytes_and_commit(")
remove_end = install_net.index("static esp_err_t remove_recover_pending", remove_start)
remove_handler = install_net[remove_start:remove_end]
commit_at = remove_handler.index("meta_carve_flash_remove_app_and_data(slot)")
app_erase_at = remove_handler.index("esp_flash_erase_region(NULL, offset, META_SIG_SECTOR)")
data_erase_at = remove_handler.index("esp_flash_erase_region(NULL, data_offsets[i], data_sizes[i])")
assert commit_at < app_erase_at < data_erase_at, "device uninstall must commit APP+DATA metadata before erasing bytes"
assert "if (commit != ESP_OK) return commit;" in remove_handler, "metadata commit failure must preserve APP/DATA bytes"
assert "nvs_set_u8(nvs, \"dc\", data_count)" in install_net, "uninstall intent must persist owned DATA extent count before commit"
assert "remove_nvs_set_u32(nvs, op, data_offsets[i])" in install_net, "uninstall intent must persist DATA offsets for post-commit recovery"
assert "remove_nvs_set_u32(nvs, sp, data_sizes[i])" in install_net, "uninstall intent must persist DATA sizes for post-commit recovery"
intent_start = install_net.index("static esp_err_t remove_intent_write(")
intent_end = install_net.index("static esp_err_t remove_intent_read(", intent_start)
intent_writer = install_net[intent_start:intent_end]
assert intent_writer.index('nvs_set_u8(nvs, "active", 0)') < intent_writer.index('nvs_set_u8(nvs, "dc", data_count)') < intent_writer.rindex('nvs_set_u8(nvs, "active", 1)'), "intent activation must happen only after the full extent snapshot is durable"
recovery_start = install_net.index("static esp_err_t remove_recover_pending(bool *did_recover)\n{")
recovery_end = install_net.index("// POST /api/install/remove", recovery_start)
recovery_handler = install_net[recovery_start:recovery_end]
assert recovery_handler.index("esp_flash_erase_region(NULL, data_offsets[i], data_sizes[i])") < recovery_handler.index("return remove_intent_clear()"), "recovery intent must remain active until every cleanup erase succeeds"
assert "Metadata was committed before power loss/erase failure" in install_net, "recovery must retry erases after metadata is already removed"
prepare_start = install_net.index("static esp_err_t h_install_prepare(")
prepare_end = install_net.index("// POST /api/install/session", prepare_start)
prepare_handler = install_net[prepare_start:prepare_end]
assert "remove_recovery_gate(&recovered_remove)" in prepare_handler, "new install must recover or block on pending uninstall"
slots_start = install_net.index("static esp_err_t h_install_slots(")
slots_end = install_net.index("static esp_err_t h_install_remove(httpd_req_t *req);", slots_start)
slots_handler = install_net[slots_start:slots_end]
assert "remove_recovery_gate(&recovered_remove)" in slots_handler, "slot listing must not expose stale state during uninstall recovery"
assert "cannot uninstall during an install session" in install_net, "uninstall must not race an active install session"
gate_start = install_net.index("static esp_err_t remove_recovery_gate(bool *did_recover)\n{")
gate_end = install_net.index("static esp_err_t remove_recover_pending(bool *did_recover)\n{", gate_start)
gate_handler = install_net[gate_start:gate_end]
assert "if (!s_remove_recovery_needed)" in gate_handler and "return ESP_OK;" in gate_handler, "steady-state DATA chunk reads must not reopen NVS for each 4 KiB request"
assert "if (e != ESP_OK) return e;" in install_net[install_net.index("static esp_err_t remove_recover_pending"):install_net.index("// POST /api/install/remove")], "failed cleanup must keep durable intent pending"
assert "const dataImages = isFullImage(buf) ? extractDataImages(buf) : [];" in usb_page, "USB local firmware must parse DATA images instead of silently discarding them"
assert "await chooseUsbDataSizes(dataImages, carveForInstall, dataPlayId)" in usb_page, "USB DATA installation must expose an optional capacity-selection step"
assert "await writeDataPayload(dataImage, dataImage)" in usb_page, "USB DATA payload must be written and read-back verified before commit"
assert 'if (isDynMode(slotModel.mode))' in usb_page and "const snapshot = await createUsbStorageSnapshot" in usb_page, "USB dynamic mode must emit raw v2 snapshots instead of the retired logical archive"
assert 'data_backup_unavailable: "Fixed-slot devices retain historical v1 APP/NVS backup. Dynamic layouts use v2 raw storage snapshots' in usb_page, "USB UI must disclose the v1 compatibility boundary and dynamic snapshot path"
assert "USB restores historical v1 APP/NVS archives on fixed-slot devices and v2 raw storage snapshots" in usb_page, "USB restore copy must disclose v1 compatibility and v2 snapshot support"
assert 'rawManifest?.kind === "meta-pass-storage-snapshot"' in usb_page and "不再接受旧 APP+DATA v2 逻辑归档" in usb_page, "USB must distinguish new snapshots from retired APP+DATA v2 archives"
assert "rawManifest?.manifest_version === 1" in usb_page and "restoreLegacyV1ArchiveUsb()" in usb_page, "USB must preserve historical v1 fixed-slot restore"
assert "async function restoreV2ArchiveUsb()" not in usb_page and "await restoreV2ArchiveUsb()" not in usb_page, "USB must not retain a destructive APP+DATA v2 restore path"
assert "rawManifest?.complete === false" in usb_page and "msg_restore_partial_archive" in usb_page, "USB restore must reject partial archives before writing any slot"
assert "manifest_version: 1" in usb_page, "USB fixed-slot backup must preserve the historical v1 archive format"
assert "archiveDownload = { url, filename }" in phone_page and "download=\"${esc(archiveDownload.filename)}\"" in phone_page, "mobile backup must expose a user-activated download link after async reads"
assert "link.click()" not in phone_page, "mobile archive export must not rely on a delayed synthetic download click"
assert "createStorageSnapshotArchive(bridge" in phone_page, "mobile v2 backup must create a whole-device snapshot"
assert "validateStorageSnapshotArchiveForDevice" in phone_page and "恢复采用逐分区擦除与写入" in phone_page, "mobile restore must preflight and use sequential partition writes"
assert "result.dataWritten" in phone_page and "请重启设备并确认 MetaPass 能正常启动" in phone_page, "mobile restore must require a successful post-restore boot check"
install_server = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
assert '"/api/install/slot/read",HTTP_GET,  h_install_slot_read' in install_server, "APP byte read endpoint must be registered"
slot_read_start = install_server.index("static esp_err_t h_install_slot_read")
slot_read_end = install_server.index("// POST /api/install/finalize", slot_read_start)
slot_read_handler = install_server[slot_read_start:slot_read_end]
for token, label in [
    ("origin_allowed(req)", "APP reads enforce Origin policy"),
    ("req_token_ok(req)", "APP reads require session authentication"),
    ('"X-Meta-Play-Id"', "APP reads identify stable ownership"),
    ("length > INSTALL_IO_BUF", "APP reads are capped at 4 KiB"),
    ("offset > match->size || length > match->size - offset", "APP reads cannot cross slot bounds"),
    ("slot->play_id == play_id && slot->kind == META_CARVE_KIND_APP", "APP reads resolve an APP slot, not DATA or storage"),
    ("slot->state != META_SLOT_EMPTY", "APP reads cannot expose an empty slot reservation"),
    ("esp_flash_read(NULL, read_buf, address, length)", "APP reads return actual flash bytes"),
    ("s_session.confirmed || s_session.session_opened", "APP reads are rejected during install activity"),
    ("install_busy", "APP reads check the install-session safety gate"),
    ("remove_recovery_gate(&recovered_remove)", "APP reads recover pending uninstall before exposing bytes"),
    ("previous uninstall recovered; refresh slot list", "APP reads stop after recovery changes slot state"),
]:
    assert token in slot_read_handler, f"missing APP read API contract: {label}"
assert "arc_records, s->play_id" in install_server, "mobile slot listing must expose APP play_id for archive ownership"
assert "arc_records, s->play_id" in install_server, "mobile slot listing must expose APP play_id for archive ownership"
assert '"/api/install/data/read",HTTP_GET,  h_install_data_read' in install_server, "DATA read endpoint must be registered"
read_start = install_server.index("static esp_err_t h_install_data_read")
read_end = install_server.index("// POST /api/install/finalize", read_start)
read_handler = install_server[read_start:read_end]
for token, label in [
    ("origin_allowed(req)", "DATA reads enforce existing Origin policy"),
    ("req_token_ok(req)", "DATA reads require session authentication"),
    ('"X-Meta-Play-Id"', "DATA reads identify APP ownership"),
    ('"X-Meta-Data-Label"', "DATA reads identify DATA label"),
    ('length > INSTALL_IO_BUF', "DATA reads are capped at 4 KiB"),
    ("offset > match->size || length > match->size - offset", "DATA reads cannot cross reservation bounds"),
    ("d->play_id == play_id && strcmp(d->label, label) == 0", "DATA reads resolve the ownership key"),
    ("esp_flash_read(NULL, read_buf, address, length)", "DATA bytes are read only after reservation lookup"),
    ("remove_recovery_gate(&recovered_remove)", "DATA reads recover pending uninstall before exposing bytes"),
    ("previous uninstall recovered; refresh slot list", "DATA reads stop after recovery changes ownership"),
]:
    assert token in read_handler, f"missing DATA read API contract: {label}"
assert "httpd_resp_set_hdr(req, \"Cache-Control\", \"no-store\")" in read_handler, "DATA read responses must not be cached"
assert "动态布局使用 v2 原始存储快照" in usb_page and "不再接受旧 APP+DATA v2 逻辑归档" in usb_page, "Chinese UI must disclose v2 snapshot support and reject the retired logical archive"
assert "commitCarve(nextCarve, { materialize: deferredDataInstall })" in usb_page, "DATA-bearing APP install must commit APP+DATA as one carve/table transaction"
assert "boot-test-and-return" in runtime and "verify-deleted" in runtime, "runtime driver must match mobile-page runner contract"
assert "dataPersistedAfterReboot" in runtime and "deletedSlotNotBootable" in runtime, "runtime driver must emit required lifecycle evidence"
assert "USB Serial/JTAG" in doc and "尚未在 ESP-IDF 环境编译" in doc, "document hardware validation limitation"
print("DATA child firmware + serial protocol contract: PASS")
