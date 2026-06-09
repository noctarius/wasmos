import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


class IrqInputTest(unittest.TestCase):
    """Verify that keyboard and mouse drivers start in IRQ-driven mode.

    irq_route_ipc(KBD_IRQ) and irq_route_ipc(MOUSE_IRQ) must succeed so
    that PS/2 events are delivered to the drivers via the kernel IPC path
    rather than falling back to polling.  This catches regressions in IRQ
    routing capability or IOAPIC configuration.
    """

    session: QemuSession

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected before timeout")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

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


if __name__ == "__main__":
    unittest.main()
