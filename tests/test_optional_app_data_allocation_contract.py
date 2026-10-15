#!/usr/bin/env python3
"""Contract tests for optional per-APP DATA allocation on USB and phone installers."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
helper = (ROOT / "install-slot/data-allocation-choice.js").read_text(encoding="utf-8")
usb = (ROOT / "install-slot/install-slot.html").read_text(encoding="utf-8")
phone = (ROOT / "install-slot/phone-install.js").read_text(encoding="utf-8")
device_model = (ROOT / "main/meta_install_model.c").read_text(encoding="utf-8")

assert "BLANK_DATA_SUBTYPE_SPIFFS = 0x82" in helper
assert 'label = `mp${(playId >>> 0).toString(16)}`' in helper
assert "if (!allocate) return [];" in helper
assert "return [makeBlankAppDataImage(playId)]" in helper
assert "0x40" not in helper, "test-only raw-sector subtype must not be generalized"

assert 'import { makeBlankAppDataImage, resolveAppDataChoice } from "./data-allocation-choice.js";' in usb
assert 'id="data-size-allocate"' in usb
usb_choice = usb[usb.index("async function chooseUsbDataSizes"):usb.index("\nasync function verifyFlashReadback")]
assert "if (!dataImages.length) return []" not in usb_choice
assert "finish({ allocate: false, sizes: [] })" in usb_choice
assert "resolveAppDataChoice({ allocate: choice.allocate" in usb
assert "planDataReservations(carveForInstall, selectedDataImages, choice.sizes, dataPlayId)" in usb

assert 'import { makeBlankAppDataImage, resolveAppDataChoice } from "./data-allocation-choice.js";' in phone
phone_choice = phone[phone.index("// DATA sizing is a general installation option"):phone.index('\n    stageEl.textContent = "上传";')]
assert "if (geom?.listing)" in phone_choice
assert 'id=mp-data-allocate type=checkbox' in phone_choice
assert "resolve([])" in phone_choice
assert "pre.offer.data = [];" in phone_choice and "pre.dataImages = [];" in phone_choice
assert "resolveAppDataChoice({ allocate: true, declaredImages: dataImages" in phone_choice
assert "options.some((o) => o.unavailable)" in phone_choice
assert "options[i].unavailable" in phone_choice
assert "pre.offer.data = [{ playId: Number(meta.play.id)" in phone_choice

assert 'if (v_initial > 0)' in device_model, "zero-payload DATA must not require an initial-image SHA"
assert 'meta_store_json_get_array_string(json, len, "data", i, "label"' in device_model
print("Optional APP DATA allocation contract: PASS")
