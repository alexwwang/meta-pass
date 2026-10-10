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


class MobilePageStorageManagementContract(unittest.TestCase):
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
        for token in ("META_PASS_E2E_SERIAL_PORT", "import serial", "Runtime driver does not exist"):
            with self.subTest(token=token):
                self.assertIn(token, runtime_preflight)
        self.assertLess(runtime_start, RUNNER.index('const urlArg = String(args.url'))

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
        self.assertIn("M06C", RUNNER[RUNNER.index("if (afterRemoveA && b"):RUNNER.index('if (b) {\n    await runCase("M07 remove B"')])

    def test_app_uninstall_cascades_data_and_ui_has_no_standalone_data_delete(self):
        for token in ("卸载 APP 必须同时删除该 APP 关联的全部 DATA",
                      "不提供单独删除 DATA 的用户操作",
                      "不允许用户选择卸载时保留 DATA",
                      "删除中断后重启",
                      "真实字节级备份未实现"):
            with self.subTest(token=token):
                self.assertIn(token, LIFECYCLE)
        installer = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
        self.assertIn("删除 APP 及全部数据", installer)
        self.assertIn("永久删除此 APP 及其全部数据", installer)
        self.assertNotIn("mp-rm-erase", installer)
        self.assertIn("不支持包含真实 DATA 字节的完整备份与恢复", installer)
        self.assertIn("旧版归档导出仅包含元数据", installer)
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
        for token in ("requireSafeStorageState", "refusing subsequent mutations",
                      "runtimeAResult?.ok", "A/B install and runtime safety gates did not pass",
                      "required DATA reservation for child A was not created",
                      "device API did not confirm install A as VALID"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)

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

    def test_document_states_evidence_boundary_and_manual_assist_gap(self):
        for token in ("PASS_WITH_MANUAL_STEPS", "真实设备结果", "不能由测试子固件的成功代替"):
            with self.subTest(token=token):
                self.assertIn(token, DESIGN)


if __name__ == "__main__":
    unittest.main(verbosity=2)
