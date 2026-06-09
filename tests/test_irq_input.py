import os
import sys
import subprocess
import shutil
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


def _qemu_display_backends() -> list:
    """Return QEMU display backends available on this host."""
    qemu_bin = shutil.which("qemu-system-x86_64") or "qemu-system-x86_64"
    try:
        result = subprocess.run(
            [qemu_bin, "-display", "help"],
            capture_output=True, text=True,
        )
        lines = result.stdout.splitlines()
        backends = []
        collecting = False
        for line in lines:
            s = line.strip().lower()
            if "available display" in s:
                collecting = True
                continue
            if collecting and s:
                if s.startswith("some display"):
                    break
                backends.append(s.split()[0])
        return backends
    except Exception:
        return []


def _best_input_display() -> str:
    """Choose a display backend that supports QMP input-send-event routing.

    Modern QEMU (≥7.0) requires an active *focused* display window to route
    HMP sendkey / input-send-event to the PS/2 i8042 controller.  In
    automated test runs the QEMU window is not focused, so these tests are
    gated behind WASMOS_TEST_INPUT_INJECTION=1.  When that variable is set,
    this function returns the best available backend; otherwise it returns ""
    so injection tests are skipped.
    """
    if not os.environ.get("WASMOS_TEST_INPUT_INJECTION"):
        return ""
    available = set(_qemu_display_backends())
    for backend in ("cocoa", "gtk", "sdl", "egl-headless"):
        if backend in available:
            return backend
    return ""


_DISPLAY_FOR_INJECTION = _best_input_display()


class IrqInputTest(unittest.TestCase):
    """Verify keyboard and mouse IRQ handling end-to-end.

    Two complementary strategies are used:

    **Boot-time delivery check** — During PS/2 controller initialization the
    mouse driver sends 8042 commands that cause a keyboard byte to appear in
    the i8042 output buffer.  This fires PS/2 IRQ 1 and the kernel delivers
    the IPC event to the keyboard driver before the CLI prompt appears.  This
    path works reliably in nographic (headless) mode and does not require
    external injection.

    **QMP injection check** (optional) — When a real display backend is
    available (e.g. cocoa, gtk), the QMP monitor can inject a synthetic
    keypress / mouse-move via HMP sendkey / mouse_move.  These commands route
    through QEMU's display input pipeline, which in QEMU ≥ 7.0 requires an
    active display context to reach the PS/2 i8042 controller.

    Serial markers emitted by the drivers:
      [keyboard] irq-event         — IPC IRQ event arrived at the kbd loop
      [keyboard] key sc=N keyup=N  — scancode decoded and dispatched
      [mouse] irq-event            — IPC IRQ event arrived at the mouse loop
      [mouse] move dx=N dy=N btn=N — 3-byte PS/2 packet assembled
    """

    session: QemuSession

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cfg.enable_monitor = True
        # Use a real display backend when available so that QMP input routing
        # can reach the PS/2 i8042 controller.  Fall back to nographic (which
        # still enables the IRQ-delivery boot-time checks).
        if _DISPLAY_FOR_INJECTION:
            cfg.nographic = False
            cfg.display = _DISPLAY_FOR_INJECTION
        # force_stop_on_timeout=False: a test timeout must not kill QEMU and
        # break the QMP socket for tests that run after the timed-out one.
        cls.session = QemuSession(cfg, timeout_s=120, echo=True,
                                  force_stop_on_timeout=False)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=120):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected before timeout")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    # ------------------------------------------------------------------
    # Startup mode checks — search the full boot buffer
    # ------------------------------------------------------------------

    def test_keyboard_started_irq_driven(self) -> None:
        """Keyboard driver must report IRQ-driven startup, not polling fallback."""
        ok = self.session.expect(b"[keyboard] driver starting (IRQ-driven)", timeout_s=1)
        if not ok:
            self.fail(
                "Keyboard driver did not start in IRQ-driven mode — "
                "irq_route_ipc(KBD_IRQ) may have failed\n"
                f"--- tail ---\n{self.session.tail()}"
            )

    def test_mouse_started_irq_driven(self) -> None:
        """Mouse driver must report IRQ-driven startup, not polling fallback."""
        ok = self.session.expect(b"[mouse] driver starting (IRQ-driven)", timeout_s=1)
        if not ok:
            self.fail(
                "Mouse driver did not start in IRQ-driven mode — "
                "irq_route_ipc(MOUSE_IRQ) may have failed\n"
                f"--- tail ---\n{self.session.tail()}"
            )

    # ------------------------------------------------------------------
    # Boot-time IRQ delivery — works in nographic/headless mode
    # ------------------------------------------------------------------

    def test_keyboard_irq_delivered_during_boot(self) -> None:
        """PS/2 IRQ 1 must be delivered to the keyboard driver during boot.

        During mouse driver initialization, 8042 controller commands cause a
        keyboard byte to appear in the PS/2 output buffer, firing IRQ 1.  The
        kernel routes this as an IPC event to the keyboard driver, which logs
        [keyboard] irq-event.  This confirms the full IRQ-over-IPC delivery
        path (IOAPIC → LAPIC → IDT vector 33 → x86_irq_handler → IPC) works.
        """
        ok = self.session.expect(b"[keyboard] irq-event", timeout_s=1)
        if not ok:
            self.fail(
                "No [keyboard] irq-event logged during boot — "
                "PS/2 IRQ 1 was never delivered through the full IOAPIC→IPC path\n"
                f"--- tail ---\n{self.session.tail()}"
            )

    # ------------------------------------------------------------------
    # QMP injection checks — require an active display backend
    # ------------------------------------------------------------------

    def test_keyboard_event_via_qmp(self) -> None:
        """Press 'a' via HMP sendkey and verify the keyboard driver logs the event.

        Skipped when no suitable display backend is available: QEMU ≥ 7.0
        requires an active display context to route HMP sendkey to the PS/2
        i8042 controller.  Run with WASMOS_QEMU_DISPLAY=cocoa|gtk|sdl to
        enable this check explicitly.

        Two serial markers are checked in order:
          [keyboard] irq-event  — IRQ 1 delivered to the driver IPC loop
          [keyboard] key        — scancode decoded and dispatched
        """
        if not _DISPLAY_FOR_INJECTION:
            self.skipTest(
                f"No display backend available for QMP input injection "
                f"(QEMU ≥ 7.0 requires cocoa/gtk/sdl; found: "
                f"{_qemu_display_backends() or ['none']})"
            )
        if self.session.monitor is None:
            self.skipTest("QMP monitor not connected")
        mark = self.session.mark()
        self.session.monitor.hmp("sendkey a")
        irq_ok = self.session.expect_from(mark, b"[keyboard] irq-event", timeout_s=10)
        if not irq_ok:
            self.fail(
                "No [keyboard] irq-event after HMP sendkey — "
                "PS/2 IRQ 1 not delivered via display backend "
                f"'{_DISPLAY_FOR_INJECTION}'\n"
                f"--- tail ---\n{self.session.tail()}"
            )
        key_ok = self.session.expect_from(mark, b"[keyboard] key ", timeout_s=5)
        if not key_ok:
            self.fail(
                "[keyboard] irq-event received but no [keyboard] key — "
                "readScancode() returned no data or the code was filtered\n"
                f"--- tail ---\n{self.session.tail()}"
            )

    def test_mouse_event_via_qmp(self) -> None:
        """Move the mouse via HMP and verify the mouse driver logs the event.

        Skipped in headless mode for the same reason as the keyboard QMP test.
        Checks:
          [mouse] irq-event  — IRQ 12 delivered to the driver IPC loop
          [mouse] move       — 3-byte PS/2 packet assembled
        """
        if not _DISPLAY_FOR_INJECTION:
            self.skipTest(
                f"No display backend available for QMP input injection "
                f"(QEMU ≥ 7.0 requires cocoa/gtk/sdl; found: "
                f"{_qemu_display_backends() or ['none']})"
            )
        if self.session.monitor is None:
            self.skipTest("QMP monitor not connected")
        mark = self.session.mark()
        self.session.monitor.hmp("mouse_move 10 0")
        irq_ok = self.session.expect_from(mark, b"[mouse] irq-event", timeout_s=10)
        if not irq_ok:
            self.fail(
                "No [mouse] irq-event after HMP mouse_move — "
                "PS/2 IRQ 12 not delivered via display backend "
                f"'{_DISPLAY_FOR_INJECTION}'\n"
                f"--- tail ---\n{self.session.tail()}"
            )
        move_ok = self.session.expect_from(mark, b"[mouse] move ", timeout_s=5)
        if not move_ok:
            self.fail(
                "[mouse] irq-event received but no [mouse] move — "
                "3-byte PS/2 packet assembly failed\n"
                f"--- tail ---\n{self.session.tail()}"
            )


if __name__ == "__main__":
    unittest.main()
