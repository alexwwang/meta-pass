#!/usr/bin/env python3
"""Guard the real-device mobile storage-management E2E against coverage regressions."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "tools/realdevice/mobile_page_e2e.mjs").read_text(encoding="utf-8")
DRIVER = (ROOT / "tools/realdevice/mobile_page_runtime_driver.py").read_text(encoding="utf-8")
SERIAL = (ROOT / "tools/realdevice/data_child_serial.py").read_text(encoding="utf-8")
DESIGN = (ROOT / "docs/assets/mobile-page-storage-e2e-design.zh_CN.md").read_text(encoding="utf-8")


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

    def test_install_uninstall_and_space_accounting_are_end_to_end(self):
        for token in ("M04 install A", "M05 install B", "assertDynamicSlotGeometry",
                      "assertBaselineDataPreserved", "M06 UI delete releases test-created DATA reservations",
                      "M08 data reservations restored to baseline", "M08 device state restored to baseline"):
            with self.subTest(token=token):
                self.assertIn(token, RUNNER)

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
