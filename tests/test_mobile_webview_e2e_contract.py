#!/usr/bin/env python3
"""Static contract for the Android Appium/WebView E2E runner."""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/realdevice/mobile_webview_e2e.mjs"
SCENARIO = ROOT / "tools/realdevice/mobile-webview-scenario.json"
source = RUNNER.read_text(encoding="utf-8")
scenario = json.loads(SCENARIO.read_text(encoding="utf-8"))

def require(needle, label):
    assert needle in source, f"missing mobile E2E contract: {label} ({needle})"

def forbid(needle, label):
    assert needle not in source, f"forbidden mobile E2E bypass: {label} ({needle})"

require('"webview"', "embedded WebView mode")
require('"browser"', "real Android Chrome mode")
require('"/contexts"', "discover actual WebView contexts")
require('"/context"', "switch into WebView context")
require('"appium:appPackage"', "launch configured native host app")
require('"appium:appActivity"', "launch configured native host activity")
require('"appium:automationName": "UiAutomator2"', "Android UI automation driver")
require('"/source"', "capture page source on failure")
require('"/screenshot"', "capture screenshot on failure")
require('"device-slots.json"', "persist independent device postcondition")
require('scenario.steps', "scenario-driven UI actions")
require('"assertJs"', "assert app page runtime state")
require('report.json', "machine-readable report")
require('finally', "session cleanup in finally")
require('"/api/install/slots"', "read-only device state verification")
for endpoint in ('"/api/install/prepare"', '"/api/install/session"',
                 '"/api/install/chunk"', '"/api/install/finalize"',
                 '"/api/install/data"', '"/api/install/remove"'):
    forbid(endpoint, "UI must drive the embedded page; no direct mutation APIs")
assert scenario["assertions"]["bodyTextMinLength"] == 1
assert any(step["action"] == "assertJs" for step in scenario["steps"])
print("Mobile WebView E2E contract: PASS")
