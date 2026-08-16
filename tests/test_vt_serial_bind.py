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


class VtSerialBindTests(unittest.TestCase):
    """The serial-bound slot is a selector of its own.

    Which slot is on screen and which slot the serial console talks to are
    independent: `tty N` moves the first, `tty -s N` moves the second
    (VT_IPC_BIND_SERIAL_REQ). Slot 0 belongs to the compositor and cannot be
    reached over a serial line, so binding to it is refused.

    Both assertions are made in one test, in order, because a successful bind is
    terminal for this session: serial input goes to a slot with no CLI on it, so
    nothing typed afterwards can be answered. That is the behaviour under test,
    not a limitation of the harness.
    """

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")
        if not cls.session.settle():
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(
                f"system never went idle after boot\n--- tail ---\n{tail}\n"
            )

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.close()

    def test_bind_refuses_gui_slot_then_moves_serial_input(self):
        mark = self.session.mark()
        self.session.send("tty -s 0")
        if not self.session.expect_from(mark, b"tty serial bind failed", timeout_s=15):
            self.fail(
                "binding the serial console to the GUI slot was not refused.\n"
                f"--- tail ---\n{self.session.tail()}\n"
            )

        # The refusal must leave the console usable: the bind failed, so serial
        # input still reaches the slot it was bound to.
        mark = self.session.mark()
        self.session.send("echo still_here")
        if not self.session.expect_from(mark, b"still_here", timeout_s=15):
            self.fail(
                "CLI stopped answering after a refused bind.\n"
                f"--- tail ---\n{self.session.tail()}\n"
            )

        mark = self.session.mark()
        self.session.send("tty -s 2")
        if not self.session.expect_from(
            mark, b"serial console bound to tty2", timeout_s=15
        ):
            self.fail(
                "binding the serial console to vt-2 was not confirmed.\n"
                f"--- tail ---\n{self.session.tail()}\n"
            )

        # Serial input now lands in vt-2, which has no reader, so this command is
        # never seen: no echo, no output. A short window is enough — the CLI
        # answers in well under a second when the input reaches it.
        mark = self.session.mark()
        self.session.send("echo unreachable_after_bind")
        self.assertFalse(
            self.session.expect_from(mark, b"unreachable_after_bind", timeout_s=8),
            msg=(
                "serial input still reached the old slot after rebinding.\n"
                f"--- tail ---\n{self.session.tail()}\n"
            ),
        )


if __name__ == "__main__":
    unittest.main()
