#!/usr/bin/env python3
"""Static contract for real-device UI, child execution, and DATA lifecycle E2E."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/realdevice/mobile_page_e2e.mjs"
source = RUNNER.read_text(encoding="utf-8")

def require(value, label):
    assert value in source, f"missing mobile page E2E contract: {label} ({value})"

def forbid(value, label):
    assert value not in source, f"forbidden direct mutation API: {label} ({value})"

require('from "./cdp_mobile_page.mjs"', "direct CDP controller; no Playwright dependency")
require('isMobile: true', "mobile browser metrics")
require('hasTouch: true', "touch emulation")
require('deviceScaleFactor: 3', "mobile DPR")
require('viewport', "configurable mobile viewport")
require('viewportMatrix', "responsive viewport matrix")
require('M01 responsive layout', "layout check at multiple phone sizes")
require('page.setViewportSize(size)', "same-session responsive viewport changes")
require('/json/version', "CDP endpoint discovery")
require('Target.attachToTarget', "direct CDP target attachment")
require('Emulation.setDeviceMetricsOverride', "mobile viewport through CDP")
require('Emulation.setTouchEmulationEnabled', "touch emulation through CDP")
require('args["cdp-url"]', "configurable remote debugging endpoint")
require('if (!args["real-device"])', "explicit real-device safety gate")
require('new CdpMobilePage', "single CDP-controlled page session")
assert "playwright" not in source, "mobile E2E runner must not depend on Playwright"
require('#mp-install-root', "actual device phone-install page")
require('#mp-q', "play search UI")
require('#mp-confirm', "install confirmation UI")
require('#mp-mgmt', "slot management UI")
require('#mp-mgmt-confirm', "delete confirmation UI")
require('M04 install A', "first install round")
require('M05 install B', "second install and coexistence")
require('M06 B remains valid after removing A', "delete isolation")
require('M08 device state restored to baseline', "baseline restore assertion")
require('/api/install/slots', "read-only independent slot verification")
require('/api/install/status', "read-only installer status verification")
require('X-Meta-Session', "session-authenticated read-only API")
require('cleanupOwned', "test-owned slot cleanup")
require('final-page.png', "failure evidence screenshot")
require('page-source.html', "page source evidence")
require('report.json', "machine-readable report")
require('redact(', "sensitive value redaction")
for endpoint in ('/api/install/prepare', '/api/install/session', '/api/install/chunk',
                 '/api/install/finalize', '/api/install/remove', '/api/install/cancel'):
    forbid(endpoint, "UI must perform all writes")
print("Mobile device-page + child-runtime + DATA lifecycle E2E contract: PASS")

require('assertDynamicSlotGeometry', "dynamic slot and DATA pool geometry assertions")
require('assertDynamicSlotGeometry(baseline, "M02 baseline")', "baseline allocation invariants")
require('assertDynamicSlotGeometry(afterA, "M04 after install A")', "first install allocation invariants")
require('assertDynamicSlotGeometry(afterB, "M05 after install B")', "second install allocation invariants")
require('assertDynamicSlotGeometry(afterRemoveA, "M06 after removing A")', "slot deletion allocation invariants")
require('assertDynamicSlotGeometry(finalSlots, "M08 final")', "final allocation invariants")
require('data reservations restored to baseline', "DATA reservation restoration")
require('baseline-data-reservations.json', "baseline DATA evidence")
require('final-data-reservations.json', "final DATA evidence")

require('runtimeDriver', "required hardware child-runtime driver")
require('boot-test-and-return', "boot real child firmware and return to launcher")
require('childBooted', "independent evidence that child firmware booted")
require('dataWriteOk', "child DATA write operation")
require('dataReadOk', "child DATA read operation")
require('dataChecksumOk', "DATA content/checksum verification")
require('dataPersistedAfterReboot', "DATA persistence across reboot")
require('returnedToLauncher', "test fixture returns to launcher before next UI step")
require('verify-deleted', "post-delete hardware verification")
require('deletedSlotNotBootable', "deleted image is no longer bootable")
require('dataPartitionReleased', "deleted child DATA partition released")
require('spawnSync(runtimeDriver', "run real hardware runtime driver")
require('requireDataReservation', "opt-in child firmware DATA reservation lifecycle scenario")
require('M05 child-firmware DATA reservation created', "verify DATA reservation creation")
require('M06 UI delete releases test-created DATA reservations', "verify DATA reservation deletion")
