#!/usr/bin/env python3
"""Static contract tests for the real USB Web UI E2E boundary."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/realdevice/usb_web_e2e.mjs"
PAGE = ROOT / "install-slot/install-slot.html"
PKG = ROOT / "tools/realdevice/package.json"
VALIDATE = ROOT / "tools/validate.sh"

runner = RUNNER.read_text(encoding="utf-8")
page = PAGE.read_text(encoding="utf-8")
pkg = PKG.read_text(encoding="utf-8")
validate = VALIDATE.read_text(encoding="utf-8")


def require(text: str, needle: str, label: str) -> None:
    assert needle in text, f"{label}: missing {needle!r}"


def forbid(text: str, needle: str, label: str) -> None:
    assert needle not in text, f"{label}: forbidden {needle!r}"


# Hardware-touch gate: a real run cannot happen accidentally.
require(runner, 'if (!argv["real-device"] && !argv.authorize)', "runner hardware gate")

# The browser must drive the page; direct install protocol paths are forbidden.
forbidden_direct = (
    "runInstall(",
    "prepareImage(",
    "createBridge(",
    '"/api/install/prepare"',
    '"/api/install/session"',
    '"/api/install/chunk"',
    '"/api/install/finalize"',
    '"/api/install/data"',
)
for needle in forbidden_direct:
    forbid(runner, needle, "runner direct-install bypass")

# Independent device verification is read-only, except recovery cleanup via remove.
require(runner, '"/api/install/slots"', "device slot verification")
require(runner, '"/api/install/status"', "device status verification")
require(runner, '"/api/install/remove"', "recovery cleanup")

# The page's real-E2E seam must still use the browser's actual Web Serial API.
require(page, 'new URLSearchParams(location.search).get("e2e") === "real"', "page E2E flag")
require(page, "navigator.serial.getPorts()", "real serial selection")
require(page, "navigator.serial.requestPort()", "normal serial selection")
require(page, "const E2E_REAL", "E2E real mode state")

# No mock profile may be combined with real E2E.
require(page, 'e2e=real cannot be combined with mock mode', "real/mock isolation")

# Dependency must be pinned rather than floating.
require(pkg, '"playwright": "1.64.0"', "Playwright version pin")

# Static validation must syntax-check the new runner.
require(validate, '"$_ck_bin" --check tools/realdevice/usb_web_e2e.mjs', "static syntax gate")

print("USB Web UI E2E contract: PASS")
