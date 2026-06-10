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
_CURSOR_START_X = 640
_CURSOR_START_Y = 400
# Screen coordinates of the "press me" button centre in the libui demo window.
# The compositor places the libui window at (104, 104): g_next_z starts at 1,
# the three animation windows take z=1..3, so libui gets z=4, off=80,
# pos=24+80=104. Content area = win + (CHROME_BORDER=1, CHROME_TITLE_H=24).
# Content-local button centre = (259, 61) → screen (105+259, 128+61) = (364, 189).
_LIBUI_BUTTON_X = 364
_LIBUI_BUTTON_Y = 189
_MOUSE_STEP = 16


class LibuiClickTest(unittest.TestCase):
    """Verify that mouse clicks are delivered to libui components end-to-end."""

    session: QemuSession
    cursor_x: int = _CURSOR_START_X
    cursor_y: int = _CURSOR_START_Y

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
        cls.cursor_x = _CURSOR_START_X
        cls.cursor_y = _CURSOR_START_Y
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

    def _move_mouse(self, dx: int, dy: int) -> None:
        """Move the pointer in smaller relative HMP steps."""
        mon = self.session.monitor
        assert mon is not None
        rem_x = dx
        rem_y = dy
        while rem_x != 0 or rem_y != 0:
            step_x = max(-_MOUSE_STEP, min(_MOUSE_STEP, rem_x))
            step_y = max(-_MOUSE_STEP, min(_MOUSE_STEP, rem_y))
            mon.hmp(f"mouse_move {step_x} {step_y}")
            self.__class__.cursor_x += step_x
            self.__class__.cursor_y += step_y
            rem_x -= step_x
            rem_y -= step_y
            time.sleep(0.02)
        time.sleep(0.05)

    def _move_to(self, x: int, y: int) -> None:
        """Move from the tracked cursor position to an absolute point."""
        dx = x - self.__class__.cursor_x
        dy = y - self.__class__.cursor_y
        self._move_mouse(dx, dy)

    def _click_current(self) -> None:
        """Left-click without moving first."""
        mon = self.session.monitor
        assert mon is not None
        mon.hmp("mouse_button 1")
        time.sleep(0.05)
        mon.hmp("mouse_button 0")
        time.sleep(0.1)

    def _move_to_libui_button(self, dx: int = 0, dy: int = 0) -> None:
        """Position the pointer over the libui button with an optional offset."""
        self._move_to(_LIBUI_BUTTON_X + dx, _LIBUI_BUTTON_Y + dy)

    def test_compositor_queues_click(self) -> None:
        """Verify compositor queues a pointer button event when mouse is clicked."""
        mon = self.session.monitor
        if mon is None:
            self.skipTest("QMP monitor not connected")

        self._move_to_libui_button()
        mark = self.session.mark()
        self._click_current()

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
        for dx, dy in [(0, 0), (10, 0), (-10, 5), (0, -5)]:
            self._move_to_libui_button(dx, dy)
            self._click_current()
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
            self._move_to_libui_button(dx, dy)
            self._click_current()
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
