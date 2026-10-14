#!/usr/bin/env python3
"""Guard the real-device mobile storage-management E2E against coverage regressions."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "tools/realdevice/mobile_page_e2e.mjs").read_text(encoding="utf-8")
DRIVER = (ROOT / "tools/realdevice/mobile_page_runtime_driver.py").read_text(encoding="utf-8")
SERIAL = (ROOT / "tools/realdevice/data_child_serial.py").read_text(encoding="utf-8")
DESIGN = (ROOT / "docs/assets/mobile-page-storage-e2e-design.zh_CN.md").read_text(encoding="utf-8")
LIFECYCLE = (ROOT / "docs/assets/app-data-lifecycle-design.zh_CN.md").read_text(encoding="utf-8")
DEVICE = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")


class MobilePageStorageManagementContract(unittest.TestCase):
    def test_restore_capability_copy_matches_supported_single_group_restore(self):
        mobile = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
        usb = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")
        self.assertIn("恢复支持完整的单个 APP+DATA 组", mobile)
        self.assertIn("归档中的 NVS 字节会校验但不会写入，目标设备现有 NVS 保持不变", mobile)
        self.assertIn("归档中的 NVS 字节只校验、不写入，目标设备现有 NVS 保持不变", mobile)
        self.assertNotIn("包含全局 NVS 的归档会被拒绝", mobile)
        self.assertNotIn("当前尚不能导入恢复", mobile)
        self.assertIn("v2 restores one complete APP+DATA group and preserves the destination device NVS", usb)
        self.assertIn("Legacy v1 restore is not supported", usb)
        self.assertNotIn("v2 APP+DATA archive restore is not implemented yet", usb)

    def test_usb_restore_matches_mobile_v2_only_contract(self):
        usb = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")
        self.assertIn("v2 restores one complete APP+DATA group and preserves the destination device NVS", usb)
        self.assertIn("legacy v1 restore is not supported", usb)
        self.assertIn("旧版 v1 归档恢复不再支持", usb)
        self.assertIn("归档中的 NVS 字节会校验但不会写入，目标设备现有 NVS 保持不变", usb)
        self.assertNotIn("multi-APP restore is not atomic", usb)
        handler_start = usb.index('$("btn-restore").addEventListener')
        handler_end = usb.index('// ===== 4. 安装 =====', handler_start)
        restore_handler = usb[handler_start:handler_end]
        self.assertIn("await restoreV2ArchiveUsb()", restore_handler)
        self.assertNotIn("msg_restore_nvs_write", restore_handler)
        self.assertNotIn("parseManifest(rawManifest)", usb[usb.index('$("restore-input").addEventListener'):handler_start])

    def test_usb_v2_restore_revalidates_single_app_data_group_before_mutation(self):
        usb = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")
        start = usb.index("async function restoreV2ArchiveUsb()")
        end = usb.index('$("btn-restore").addEventListener', start)
        restore = usb[start:end]
        for token in (
            "prepareDataArchiveRestore(manifest, payloads",
            "const archivedSlot = prepared.slot",
        ):
            with self.subTest(token=token):
                self.assertIn(token, restore)
        shared = (ROOT / "install-slot/data-byte-archive.js").read_text(encoding="utf-8")
        for token in (
            "export function validateSingleGroupRestoreManifest(manifest)",
            "manifest.complete !== true",
            "manifest.slots.length !== 1",
            "record.play_id !== slot.play_id",
        ):
            with self.subTest(token=token):
                self.assertIn(token, shared)
        preparation = (ROOT / "install-slot/data-archive-restore.js").read_text(encoding="utf-8")
        for token in (
            "validateSingleGroupRestoreManifest(manifest)",
            "validateDataArchivePayloads(manifest, payloads, sha256Hex)",
            "DATA payload length is not aligned to the allocation granule",
        ):
            with self.subTest(token=token):
                self.assertIn(token, preparation)
        self.assertLess(restore.index("prepareDataArchiveRestore(manifest, payloads"),
                        restore.index("await loadSlotModel()"))

    def test_mobile_viewport_and_page_health_are_checked(self):
        for token in ("M01 no horizontal page overflow", "responsive layout", "no console errors",
                      "no failed network requests", "320x720,360x800,390x844,430x932"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)

    def test_baseline_is_a_hard_safety_gate(self):
        for token in ("baselineIdle", "baselineGeometry", "refusing to mutate device",
                      "baseline-slots.json", "baseline-data-reservations.json"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)

    def test_runtime_driver_uses_line_terminated_serial_commands(self):
        # Rejection probes must send the same newline-delimited protocol as
        # successful commands; a literal backslash+n leaves the child waiting.
        self.assertIn('command + "\\n"', DRIVER)
        self.assertNotIn('command + "\\\\n"', DRIVER)

    def test_runtime_driver_prerequisites_fail_before_hardware_mutation(self):
        runtime_start = RUNNER.index("if (runtimeDriver) {")
        runtime_end = RUNNER.index("if (manualAssist &&", runtime_start)
        runtime_preflight = RUNNER[runtime_start:runtime_end]
        for token in ("META_PASS_E2E_SERIAL_PORT", "serialPort.includes(path.sep)",
                      "does not exist; refusing device mutation", "import serial", "Runtime driver does not exist"):
            with self.subTest(token=token):
                self.assertIn(token, runtime_preflight)
        self.assertLess(runtime_start, RUNNER.index('const urlArg = String(args.url'))

    def test_realdevice_requires_explicit_dedicated_play_ids_and_data_reservations(self):
        self.assertIn('process.env.MOBILE_E2E_PLAY_A || ""', RUNNER)
        self.assertIn('process.env.MOBILE_E2E_PLAY_B || ""', RUNNER)
        self.assertIn("there are no default IDs", RUNNER)
        self.assertIn("Pass --require-data-reservation", RUNNER)
        self.assertNotIn('MOBILE_E2E_PLAY_A || "1"', RUNNER)
        self.assertNotIn('MOBILE_E2E_PLAY_B || "2"', RUNNER)

    def test_install_uninstall_and_space_accounting_are_end_to_end(self):
        for token in ("M04 install A", "M05 install B", "assertDynamicSlotGeometry",
                      "assertBaselineDataPreserved", "M06 UI delete releases test-created DATA reservations",
                      "M08 data reservations restored to baseline", "M08 device state restored to baseline"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)


    def test_existing_data_resize_never_offers_shrinking(self):
        installer = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
        self.assertIn("const listingWithoutTarget = (i) =>", installer)
        self.assertIn("Math.max(declaredMin, existingSizes[i])", installer)
        self.assertIn("existingSizes[i] != null && editable[i]", installer)

    def test_mobile_data_read_api_is_authenticated_bounded_and_identity_scoped(self):
        start = DEVICE.index("static esp_err_t h_install_data_read(")
        end = DEVICE.index("// POST /api/install/finalize", start)
        handler = DEVICE[start:end]
        for token in ("origin_allowed(req)", "req_token_ok(req)",
                      '"X-Meta-Play-Id"', '"X-Meta-Data-Label"',
                      "length > INSTALL_IO_BUF", "d->play_id == play_id",
                      'strcmp(d->label, label) == 0',
                      "offset > match->size || length > match->size - offset",
                      "esp_flash_read(NULL, read_buf, address, length)",
                      '"Cache-Control", "no-store"'):
            with self.subTest(token=token):
                self.assertIn(token, handler)
        self.assertIn('{ "/api/install/data/read",HTTP_GET,  h_install_data_read,NULL }', DEVICE)
        self.assertIn('dataRead: (playId, label, offset, length)', (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8"))
        self.assertIn('{ "/api/install/slot/read",HTTP_GET,  h_install_slot_read,NULL }', DEVICE)
        slot_start = DEVICE.index("static esp_err_t h_install_slot_read")
        slot_end = DEVICE.index("// POST /api/install/finalize", slot_start)
        slot_handler = DEVICE[slot_start:slot_end]
        for token in ("origin_allowed(req)", "req_token_ok(req)", '"X-Meta-Play-Id"',
                      "length > INSTALL_IO_BUF", "offset > match->size || length > match->size - offset",
                      "slot->play_id == play_id && slot->kind == META_CARVE_KIND_APP",
                      "esp_flash_read(NULL, read_buf, address, length)"):
            with self.subTest(token=token):
                self.assertIn(token, slot_handler)
        self.assertIn('slotRead: (playId, offset, length)', (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8"))

    def test_in_place_data_growth_erases_only_new_free_tail(self):
        device = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
        start = device.index("static esp_err_t prepare_data_moves_locked(")
        end = device.index("static void cleanup_migrated_sources", start)
        migration = device[start:end]
        self.assertIn("if (now->size > old->size)", migration)
        self.assertIn("const uint64_t tail_start64 = (uint64_t)old->offset + old->size;", migration)
        self.assertIn("if (tail_start64 > UINT32_MAX) return ESP_ERR_INVALID_STATE;", migration)
        self.assertIn("const uint32_t tail_offset = (uint32_t)tail_start64;", migration)
        self.assertIn("in-place DATA growth tail overlaps existing DATA source", migration)
        self.assertIn("in-place DATA growth tails overlap", migration)
        self.assertIn("in-place DATA growth tail overlaps migration target", migration)
        self.assertIn("for (uint8_t j = 0; j < i; j++)", migration)
        self.assertIn("esp_flash_erase_region(NULL, grow_offset[i], grow_size[i])", migration)
        self.assertLess(migration.index("All destructive operations start only after the full preflight passes"),
                        migration.index("esp_flash_erase_region(NULL, grow_offset[i], grow_size[i])"))

    def test_data_migration_preflights_all_destinations_before_erasing(self):
        device = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
        start = device.index("static esp_err_t prepare_data_moves_locked(")
        end = device.index("static void cleanup_migrated_sources", start)
        migration = device[start:end]
        self.assertIn("data_ranges_overlap", migration)
        self.assertIn("data migration target overlaps existing DATA source", migration)
        self.assertIn("data migration targets overlap", migration)
        self.assertLess(migration.index("All destructive operations start only after the full preflight passes"),
                        migration.index("esp_flash_erase_region(NULL, moves[i].new_offset"))
        self.assertIn("uint64_t a_end", device[device.index("static bool data_ranges_overlap"):start])

    def test_every_new_install_requires_a_fresh_dynamic_app_carve(self):
        installer = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
        device = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
        design = (ROOT / "docs/assets/dynslot-install-data-wiring-design.zh_CN.md").read_text(encoding="utf-8")
        self.assertIn("无法安全创建新的 APP 槽位", installer)
        self.assertIn("const opts = geom.proposal", installer)
        self.assertIn("fresh APP carve required", device)
        self.assertIn("每次安装都必须从动态回收池创建新的 APP carve", design)

    def test_realdevice_e2e_reinstalls_after_delete_using_a_fresh_app_carve(self):
        for token in ("const nameC =", "M06C fresh install after deletion",
                      "new APP carve, no stale A slot revival",
                      "M06C fresh DATA reservation for reinstalled play",
                      "M06C remove C and verify B isolation"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)
        self.assertIn("const ownedNames = new Set([nameA, nameB, nameC])", RUNNER)
        self.assertIn("M06C fresh DATA reservations released", RUNNER)
        self.assertIn("M06C C APP slot removed", RUNNER)
        self.assertIn("M06C B DATA reservations preserved", RUNNER)
        self.assertIn("freshReservationsForB.every", RUNNER)
        self.assertIn("freshReservationsForC.every", RUNNER)
        self.assertIn("M06C", RUNNER[RUNNER.index("M06C fresh install after deletion"):RUNNER.index('if (b) {\n    await runCase("M07 remove B"')])

    def test_app_uninstall_cascades_data_and_ui_has_no_standalone_data_delete(self):
        for token in ("卸载 APP 必须同时删除该 APP 关联的全部 DATA",
                      "不提供单独删除 DATA 的用户操作",
                      "不允许用户选择卸载时保留 DATA",
                      "删除中断后重启",
                      "UI 支持完整单 APP+DATA v2 归档恢复"):
            with self.subTest(token=token):
                self.assertIn(token, LIFECYCLE)
        installer = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
        self.assertIn("删除 APP 及全部数据", installer)
        self.assertIn("永久删除此 APP 及其全部数据", installer)
        self.assertNotIn("mp-rm-erase", installer)
        self.assertIn("可导出所选 APP 槽位及关联 DATA 的真实字节", installer)
        self.assertIn("export async function restoreDataArchive", installer)
        self.assertIn("readDataArchiveZip", installer)
        self.assertIn("可在空间管理中恢复单个完整 APP+DATA 组", installer)
        self.assertIn("readAppSlotReservation", installer)
        self.assertIn("readDataReservation", installer)
        self.assertIn("validateDataArchivePayloads(manifest, payloadMap, sha256Hex)", installer)
        self.assertNotIn("data-export-pid", installer)
        device = (ROOT / "main/meta_store_install.c").read_text(encoding="utf-8")
        model = (ROOT / "main/meta_install_model.c").read_text(encoding="utf-8")
        self.assertIn("REMOVE_NVS_NS", device)
        self.assertIn("remove_intent_write", device)
        self.assertIn("remove_recover_pending", device)
        self.assertIn("remove_app_bytes_and_commit", device)
        self.assertIn("meta_carve_flash_remove_app_and_data", device)
        self.assertIn("DATA ownership is unknown", device)
        # An unresolved durable uninstall intent must prevent the management
        # HTTP API from starting; otherwise another mutation could invalidate
        # the recovery target before the delete transaction is resumed.
        startup = device[device.index("esp_err_t meta_install_net_start(void)"):]
        recovery_guard = startup.index("pending uninstall recovery failed")
        server_start = startup.index("httpd_start(&s_httpd, &hcfg)")
        self.assertLess(recovery_guard, server_start)
        self.assertIn("return recovery;", startup[recovery_guard:server_start])
        self.assertIn("501 Not Implemented", device)
        self.assertIn("metadata-only import is disabled", device)
        self.assertIn("out->erase_data = true", model)

    def test_child_b_reservation_is_attributed_to_b_play_id(self):
        self.assertIn("d.play_id === Number(playB)", RUNNER)
        self.assertIn('"playId=" + Number(playB)', RUNNER)

    def test_storage_invariant_failures_block_follow_on_mutations(self):
        for token in ("requireSafeStorageState", "assertBaselineAppSlotsPreserved",
                      "baseline APP slots preserved", "refusing subsequent mutations",
                      "runtimeAResult?.ok", "A/B install and runtime safety gates did not pass",
                      "required DATA reservation for child A was not created",
                      "device API did not confirm install A as VALID",
                      "installer did not return to idle after install; refusing further device mutations",
                      "A/B DATA physical isolation could not be verified; refusing further device mutations"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)

    def test_data_cleanup_or_survivor_failure_blocks_follow_on_mutations(self):
        for token in ("bPreservedAfterA", "removing A affected B APP slot; refusing further device mutations",
                      "removing A did not release all expected test-created DATA reservations; refusing further device mutations"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)

    def test_reinstall_c_requires_all_remove_a_checks_to_pass(self):
        self.assertIn('let removeAResult = { ok: false, value: null }', RUNNER)
        self.assertIn('removeAResult = await runCase("M06 remove A and verify B isolation"', RUNNER)
        c_gate = RUNNER[RUNNER.index("if (removeAResult.ok && afterRemoveA"):RUNNER.index('if (b) {\n    await runCase("M07 remove B"')]
        self.assertIn("removeAResult.ok", c_gate)

    def test_cancelled_uninstall_must_leave_state_unchanged(self):
        for token in ("cancelRemoveByName", "cancel uninstall is a no-op",
                      "slotsUnchanged", "installerIdle", "#mp-mgmt-x"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)

    def test_runtime_evidence_cannot_be_replaced_by_ui_toast(self):
        for token in ("childBooted", "dataEraseOk", "dataWriteOk", "dataReadOk",
                      "dataChecksumOk", "dataPersistedAfterReboot", "returnedToLauncher",
                      "serialEvidence", "deletedSlotNotBootable", "dataPartitionReleased"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)
        self.assertIn("serial", DRIVER.lower())
        self.assertIn("checksum", SERIAL.lower())

    def test_deleted_runtime_evidence_must_match_requested_identity(self):
        verifier = RUNNER[RUNNER.index("function verifyDeletedRuntime"):RUNNER.index("async function cleanupOwned")]
        for token in ("evidence.playId === Number(playId)", "evidence.slotName === name",
                      "evidence.serialEvidence.trim().length > 0",
                      "delete postcondition or evidence identity failed"):
            with self.subTest(token=token):
                self.assertIn(token, verifier)

    def test_failure_cleanup_is_followed_by_independent_restoration_readback(self):
        cleanup = RUNNER[RUNNER.index("async function cleanupOwned"):RUNNER.index("try {\n  page = new CdpMobilePage")]
        for token in ("Only touch names generated by this run", "cleanup.failures",
                      "async function verifyDeviceRestoration", "readSlots()", "readStatus()",
                      "slotsRestored", "dataRestored", "installerIdle", "geometryValid",
                      '"RESTORED"', '"NOT_RESTORED"', '"UNKNOWN"',
                      "FAIL_DEVICE_NOT_RESTORED", "recovery-final-slots.json",
                      "recovery-final-status.json"):
            with self.subTest(token=token):
                self.assertIn(token, cleanup + RUNNER[RUNNER.index("async function verifyDeviceRestoration"):])
        self.assertIn("await verifyDeviceRestoration();", RUNNER)
        self.assertLess(RUNNER.index("await cleanupOwned();"), RUNNER.index("await verifyDeviceRestoration();"))
        self.assertIn("device restoration invariant", RUNNER)

    def test_document_states_evidence_boundary_and_manual_assist_gap(self):
        for token in ("PASS_WITH_MANUAL_STEPS", "真实设备结果", "不能由测试子固件的成功代替"):
            with self.subTest(token=token):
                self.assertIn(token, DESIGN)

    def test_document_has_explicit_local_realdevice_runbook(self):
        for token in ("MOBILE_E2E_URL", "META_PASS_SESSION", "META_PASS_E2E_SERIAL_PORT",
                      "--require-data-reservation", "CONFIG_META_E2E_TEST_CONTROL=y",
                      "Node.js 22", "report.json"):
            with self.subTest(token=token):
                self.assertIn(token, DESIGN)


    def test_mobile_nvs_reads_are_fixed_partition_authenticated_and_bounded(self):
        mobile = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
        for token in ('"/api/install/nvs/info"', '"/api/install/nvs/read"',
                      "nvsInfo: () => call", "nvsRead: (offset, length)",
                      "length > 4096", 'nvs: { file: "nvs.bin"',
                      "正在备份系统 NVS", "NVS 只校验、不写入"):
            with self.subTest(token=token):
                self.assertIn(token, mobile)
        for token in ("h_install_nvs_info", "h_install_nvs_read",
                      "ESP_PARTITION_SUBTYPE_DATA_NVS, \"nvs\"",
                      "origin_allowed(req)", "req_token_ok(req)",
                      "length > INSTALL_IO_BUF", "esp_partition_read(part, offset, read_buf, length)",
                      "offset > part->size || length > part->size - offset",
                      '"/api/install/nvs/info"', '"/api/install/nvs/read"'):
            with self.subTest(token=token):
                self.assertIn(token, DEVICE)
        usb = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")
        self.assertIn("Backup ZIP files are unencrypted", usb)
        self.assertIn("备份 ZIP 未加密", usb)
        self.assertIn("备份 ZIP 未加密", mobile)


if __name__ == "__main__":
    unittest.main(verbosity=2)
