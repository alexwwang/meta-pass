#!/usr/bin/env python3
"""Static contract for the non-mutating browser-only mobile viewport smoke."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "tools/realdevice/mobile_page_browser_smoke.mjs").read_text(encoding="utf-8")
VALIDATE = (ROOT / "tools/validate.sh").read_text(encoding="utf-8")

for token in (
    'from "./cdp_mobile_page.mjs"',
    "viewportMatrix",
    "Emulation",
    "#mp-install-root",
    "#mp-q",
    "#mp-mgmt",
    "no horizontal overflow",
    "no uncaught page errors",
    "no console errors",
    "no failed network requests",
    "report.json",
    "never clicks controls or calls write APIs",
):
    assert token in RUNNER, f"browser smoke missing contract: {token}"

for forbidden in (
    "/api/install/prepare", "/api/install/session", "/api/install/chunk",
    "/api/install/finalize", "/api/install/remove", "/api/install/cancel",
):
    assert forbidden not in RUNNER, f"browser-only smoke must not call mutating API: {forbidden}"

assert "tests/test_mobile_page_browser_smoke_contract.py" in VALIDATE
assert "tools/realdevice/mobile_page_browser_smoke.mjs" in VALIDATE
print("Mobile browser-only viewport smoke contract: PASS")
