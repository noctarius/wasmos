"""
test_libui_click.py — verify that mouse clicks reach libui components.

Boots gfx_smoke, waits for the libui demo window, then injects mouse
movement and clicks using HMP (which routes through the PS/2 controller).
Checks the diagnostic markers added to pump_libui_demo and the compositor.

Requires WASMOS_TEST_INPUT_INJECTION=1 and a display backend.
"""
import os
import sys
import subprocess
import shutil
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
        result = subprocess.run([qemu_bin, "-display", "help"],
                                capture_output=True, text=True)
        for backend in ("cocoa", "gtk", "sdl"):
            if backend in result.stdout.lower():
                return backend
    except Exception:
        pass
    return ""


_DISPLAY = _best_display()


class LibuiClickTest(unittest.TestCase):
    """Verify that mouse clicks are delivered to libui components end-to-end."""

    session: QemuSession

    @classmethod
    def setUpClass(cls) -> None:
        if not _DISPLAY:
            return
        cfg = default_config()
        cfg.enable_monitor = True
        cfg.nographic = False
        cfg.display = _DISPLAY
        cls.session = QemuSession(cfg, timeout_s=180, echo=True,
                                  force_stop_on_timeout=False)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=120):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected")
        cls.session.send("spawn gfx_smoke")
        if not cls.session.expect(b"[test] libui demo ready", timeout_s=30):
            cls.session.close()
            raise RuntimeError("libui demo did not start")
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

    def _click_at(self, dx: int, dy: int) -> None:
        """Move mouse by (dx,dy) from current position then left-click."""
        mon = self.session.monitor
        assert mon is not None
        if dx != 0 or dy != 0:
            mon.hmp(f"mouse_move {dx} {dy}")
            time.sleep(0.05)
        # button down then up
        mon.hmp("mouse_button 1")
        time.sleep(0.05)
        mon.hmp("mouse_button 0")
        time.sleep(0.1)

    def test_compositor_queues_click(self) -> None:
        """Verify compositor queues a pointer button event when mouse is clicked."""
        mon = self.session.monitor
        if mon is None:
            self.skipTest("QMP monitor not connected")

        # The cursor starts at screen centre (640,400 for 1280×800).
        # The libui window (4th window, z≈4) spawns at approx (104,104).
        # Move cursor toward the libui window's content area.
        # dx ≈ 104 + 260 - 640 = -276, dy ≈ 104 + 85 - 400 = -211
        mark = self.session.mark()
        self._click_at(-276, -211)

        ok = self.session.expect_from(mark, b"[dbg-gfx] pointer btn-push queued",
                                       timeout_s=5)
        if not ok:
            self.fail(
                "Compositor did not queue a pointer button-press event.\n"
                "PS/2 click may not be reaching the compositor.\n"
                f"--- tail ---\n{self.session.tail()}"
            )

    def test_libui_receives_click(self) -> None:
        """Verify libui receives the pointer event after compositor queues it."""
        mon = self.session.monitor
        if mon is None:
            self.skipTest("QMP monitor not connected")

        mark = self.session.mark()
        # Try clicking the button a few times at slightly different positions
        # in case we missed on the first attempt.
        for dx, dy in [(0, 0), (10, 0), (-10, 5), (0, -5)]:
            self._click_at(dx, dy)
            if self.session.expect_from(mark, b"[dbg-libui] pointer btn-down",
                                         timeout_s=3):
                break
        else:
            self.fail(
                "pump_libui_demo never received a pointer button-down event.\n"
                "The event may be queued but not consumed, or the libui "
                "window may not be in focus.\n"
                f"--- tail ---\n{self.session.tail()}"
            )

    def test_on_click_fires(self) -> None:
        """Verify the libui button on_click callback fires on complete click."""
        mon = self.session.monitor
        if mon is None:
            self.skipTest("QMP monitor not connected")

        mark = self.session.mark()
        for dx, dy in [(0, 0), (10, 0), (-10, 5), (5, 5)]:
            self._click_at(dx, dy)
            if self.session.expect_from(mark, b"[dbg-libui] on_click fired",
                                         timeout_s=3):
                return
        self.fail(
            "on_click callback was never fired after multiple click attempts.\n"
            "The press/release state machine in ui_loop_handle_ipc may be broken.\n"
            f"--- tail ---\n{self.session.tail()}"
        )


if __name__ == "__main__":
    unittest.main()
