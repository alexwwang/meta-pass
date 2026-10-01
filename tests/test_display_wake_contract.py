#!/usr/bin/env python3
"""Static contracts for the deep-sleep wake path.

Background: a slot firmware may put itself into deep sleep after an idle
timeout. Pressing a key then performs a full bootloader start. Three things
must hold for the user to land back in that firmware with a lit screen:

1. The launcher's BSP must recover a panel that is still in Sleep In and must
   release the pin holds the slot firmware left behind. Otherwise the backlight
   (a separate LEDC channel) comes up while every SPI init command is swallowed
   -> backlight on, screen black.
2. The launcher must keep the rollback machinery enabled, so that booting a
   slot runs it as a trial and its otadata copy is left in PENDING_VERIFY.
3. On a deep-sleep wake the bootloader hook must renew that PENDING_VERIFY copy
   to VALID, so the bootloader resumes the slot instead of rolling back to the
   launcher. IDF's own fast boot cannot do this here: it stores the resume
   target in RTC fast memory, which a child firmware's link script does not
   reserve (see the sdkconfig.defaults note).

These are static contracts over the sources; they pin ordering, config, and the
hook wiring, not register-level hardware behavior.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def initializer(source: str, symbol: str) -> str:
    match = re.search(rf"\b{re.escape(symbol)}\s*\[\]\s*=\s*\{{", source)
    if not match:
        raise AssertionError(f"initializer not found: {symbol}")
    end = source.find("};", match.end())
    if end < 0:
        raise AssertionError(f"initializer is unterminated: {symbol}")
    return source[match.end():end]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


class DisplayWakeContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.display = read("components/bsp/src/bsp_display.c")
        cls.sdkconfig = read("sdkconfig.defaults")
        cls.hooks = read("bootloader_components/meta_boot_hooks/hooks.c")

    def test_lcd_safe_levels_and_pins_are_declared(self) -> None:
        pins = initializer(self.display, "s_deep_sleep_pins")
        levels = initializer(self.display, "s_deep_sleep_levels")
        self.assertRegex(
            pins,
            r"BSP_LCD_CS.*BSP_LCD_SCLK.*BSP_LCD_MOSI.*BSP_LCD_DC.*BSP_LCD_BL",
        )
        self.assertRegex(levels, r"1\s*,\s*0\s*,\s*0\s*,\s*0\s*,\s*0")

    def test_holds_are_released_in_a_glitch_free_order(self) -> None:
        release = function_body(self.display, "display_release_deep_sleep_holds")
        # Global hold off first, then write safe levels while the per-pin holds
        # still latch them, then release each pin.
        self.assertIn("gpio_deep_sleep_hold_dis", release)
        self.assertIn("display_set_safe_levels()", release)
        self.assertIn("gpio_hold_dis", release)
        self.assertLess(release.index("gpio_deep_sleep_hold_dis"),
                        release.index("gpio_hold_dis"))

    def test_holds_are_released_before_spi_takes_the_pins(self) -> None:
        init = function_body(self.display, "bsp_display_init")
        self.assertIn("display_release_deep_sleep_holds()", init)
        self.assertLess(init.index("display_release_deep_sleep_holds()"),
                        init.index("spi_bus_initialize"))

    def test_slpout_precedes_panel_reset(self) -> None:
        # The panel may still be in Sleep In; SWRESET on a stopped-oscillator
        # panel is both ineffective and able to deadlock its command decoder.
        init = function_body(self.display, "bsp_display_init")
        slpout = init.index("0x11")
        self.assertLess(slpout, init.index("esp_lcd_new_panel_st7789"))
        self.assertLess(slpout, init.index("esp_lcd_panel_reset"))
        tail = init[slpout:]
        self.assertRegex(tail, r"vTaskDelay\(pdMS_TO_TICKS\(120\)\)")

    def test_deep_sleep_resume_stays_in_the_bootloader_hook(self) -> None:
        # Trial-run rollback must stay on: it is what leaves the slot's otadata
        # copy in PENDING_VERIFY while the slot runs, which the resume path keys
        # on. Fast boot by RTC memory stays off: a child firmware's link script
        # does not reserve the bootloader's retain area, so that route silently
        # falls back and needs every child firmware to cooperate.
        self.assertRegex(self.sdkconfig,
                         r"(?m)^CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\s*$")
        self.assertNotRegex(
            self.sdkconfig,
            r"(?m)^CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y\s*$",
        )
        after_init = function_body(self.hooks, "bootloader_after_init")
        self.assertIn("RESET_REASON_CORE_DEEP_SLEEP", after_init)
        self.assertIn("resume_running_slot_on_copy", after_init)
        # Cold resets keep the erase policy; only the deep-sleep branch resumes.
        self.assertIn("enforce_single_session_on_copy", after_init)
        self.assertLess(after_init.index("RESET_REASON_CORE_DEEP_SLEEP"),
                        after_init.index("enforce_single_session_on_copy"))

    def test_resume_checks_crc_and_erases_before_writing(self) -> None:
        # A corrupt copy must be left to the bootloader's own fallback, and the
        # state byte can only go 0x1 -> 0x2 after the sector is erased (flash
        # programming is one-way).
        resume = function_body(self.hooks, "resume_running_slot_on_copy")
        self.assertIn("meta_boot_policy_entry_must_resume", resume)
        self.assertIn("bootloader_common_ota_select_crc", resume)
        self.assertLess(resume.index("bootloader_flash_erase_sector"),
                        resume.index("bootloader_flash_write"))


if __name__ == "__main__":
    unittest.main()
