"""
test_gfx_keyboard.py — verify that keyboard input reaches a focused gfx window.

Boots the GUI, focuses gfx_smoke, injects a key via the QEMU monitor (which
routes through the emulated PS/2 controller), and checks that the keypress is
delivered to the focused window as a GFX_EVENT_KEY.  gfx_smoke echoes each key
event to serial as "[test] gfx smoke event key sc=<code> flags=<flags>", so the
serial stream is the observable channel — no app changes needed.

This guards the keyboard input path (keyboard driver -> VT -> compositor ->
focused window) across the VT single-decoder work: the key must still reach the
gfx app after keyboard routing is funneled through the VT.

Requires WASMOS_TEST_INPUT_INJECTION=1 and a display backend (cocoa/gtk/sdl);
skips otherwise, like test_libui_click.
"""

import os
import shutil
import subprocess
import sys
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


def _best_display() -> str:
    if not os.environ.get("WASMOS_TEST_INPUT_INJECTION"):
        return ""
    qemu_bin = shutil.which("qemu-system-x86_64") or "qemu-system-x86_64"
    try:
        result = subprocess.run(
            [qemu_bin, "-display", "help"], capture_output=True, text=True
        )
        for backend in ("cocoa", "gtk", "sdl"):
            if backend in result.stdout.lower():
                return backend
    except Exception:
        pass
    return ""


_DISPLAY = _best_display()
# The compositor inits the pointer at the framebuffer centre (1280x800).
_CURSOR_START_X = 640
_CURSOR_START_Y = 400
# Title bar of the auto-started gfx_smoke's third animation window ("gfx smoke
# 3"), verified by screendump.  Clicking a title bar raises+focuses that window,
# and its key events are the ones gfx_smoke echoes as "gfx smoke event key" from
# its steady-state wait loop.  (The libui demo window, which is frontmost by
# default, routes keys to its own text-input component instead — not observable.)
_ANIM_TITLE_X = 400
_ANIM_TITLE_Y = 135
_MOUSE_STEP = 16


class GfxKeyboardTest(unittest.TestCase):
    """Verify keyboard input is delivered to the focused gfx window."""

    session: QemuSession

    @classmethod
    def setUpClass(cls) -> None:
        if not _DISPLAY:
            return
        cfg = default_config()
        cfg.enable_monitor = True
        cfg.nographic = False
        cfg.display = _DISPLAY
        cls.session = QemuSession(
            cfg, timeout_s=180, echo=True, force_stop_on_timeout=False
        )
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=120):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected")
        # Use the auto-started gfx_smoke; wait until it is in its steady-state
        # close-request wait loop (where it echoes key events to serial).
        if not cls.session.expect(b"gfx smoke waiting close-request", timeout_s=45):
            cls.session.close()
            raise RuntimeError("gfx_smoke did not reach its wait loop")
        time.sleep(0.5)

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "session") and cls.session:
            cls.session.send("halt")
            cls.session.close()

    def setUp(self) -> None:
        if not _DISPLAY:
            self.skipTest(
                "Set WASMOS_TEST_INPUT_INJECTION=1 with cocoa/gtk/sdl display."
            )

    def _click_anim_window(self) -> None:
        """Raise+focus a gfx_smoke animation window by clicking its title bar."""
        mon = self.session.monitor
        assert mon is not None
        rem_x = _ANIM_TITLE_X - _CURSOR_START_X
        rem_y = _ANIM_TITLE_Y - _CURSOR_START_Y
        while rem_x != 0 or rem_y != 0:
            step_x = max(-_MOUSE_STEP, min(_MOUSE_STEP, rem_x))
            step_y = max(-_MOUSE_STEP, min(_MOUSE_STEP, rem_y))
            mon.hmp(f"mouse_move {step_x} {step_y}")
            rem_x -= step_x
            rem_y -= step_y
            time.sleep(0.02)
        mon.hmp("mouse_button 1")
        time.sleep(0.05)
        mon.hmp("mouse_button 0")
        time.sleep(0.2)

    def test_keypress_reaches_focused_window(self) -> None:
        mon = self.session.monitor
        if mon is None:
            self.skipTest("QMP monitor not connected")

        self._click_anim_window()
        mark = self.session.mark()
        # Inject a few keys; any one delivered as a GFX_EVENT_KEY proves the
        # keyboard -> compositor -> focused window path works.
        for key in ("a", "b", "spc", "ret", "x", "y"):
            mon.hmp(f"sendkey {key}")
            time.sleep(0.15)
            if self.session.expect_from(
                mark, b"[test] gfx smoke event key", timeout_s=3
            ):
                return
        self.fail(
            "No GFX_EVENT_KEY reached gfx_smoke after injecting keys.\n"
            "Keyboard input is not being delivered to the focused window.\n"
            f"--- tail ---\n{self.session.tail()}"
        )


if __name__ == "__main__":
    unittest.main()
