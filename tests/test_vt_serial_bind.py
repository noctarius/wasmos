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

    Binding to a slot that has no shell is not a dead end: the vt spawns a CLI
    pinned to that slot on the bind, so the serial line gets a prompt from the
    new shell. Serial carries the bound slot's output only, so what a shell on
    any other slot prints never reaches it. The assertions run in one test, in
    order, because each one moves the session's state.
    """

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        cls.session.require_prompt()
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

        # vt-2 had no shell, so the bind creates one pinned to it. Its banner is
        # how both the bind and the spawn are observable on serial: vt-1's shell
        # prints its own confirmation to vt-1, which is no longer the slot serial
        # carries.
        bind = self.session.send_command("tty -s 2", timeout_s=15)
        if not self.session.expect_from(bind, b"WAMOS CLI", timeout_s=30):
            self.fail(
                "no CLI was spawned for the newly bound slot.\n"
                f"--- tail ---\n{self.session.tail()}\n"
            )

        # Serial input now reaches vt-2, and the shell answering it is the new
        # one: the session stays usable across a rebind.
        mark = self.session.mark()
        self.session.send("echo bound_slot_alive")
        if not self.session.expect_from(mark, b"bound_slot_alive", timeout_s=20):
            self.fail(
                "the newly bound slot's CLI did not answer.\n"
                f"--- tail ---\n{self.session.tail()}\n"
            )

        # Serial carries the bound slot's output and nothing else. vt-2's shell
        # moves serial back to vt-1; the vt rebinds before it replies, so the
        # shell's confirmation is written to vt-2 while serial belongs to vt-1
        # and must not reach the wire. Before a process's output was routed to
        # its own slot it went to the kernel log and from there to COM1, so it
        # showed up here whatever the binding.
        start = self.session.send_command("tty -s 1", timeout_s=20)
        probe = self.session.send_command("echo back_on_one", timeout_s=20)
        if not self.session.expect_from(probe, b"back_on_one", timeout_s=20):
            self.fail(
                "vt-1's shell did not answer after serial was bound back to it.\n"
                f"--- tail ---\n{self.session.tail()}\n"
            )
        if not self.session.expect_from(probe, b"wamos> ", timeout_s=20):
            self.fail(
                f"no prompt after the probe on vt-1.\n--- tail ---\n{self.session.tail()}\n"
            )
        self.assertNotIn(
            b"serial console bound to tty1",
            bytes(self.session.buf[start:]),
            "vt-2's shell output reached serial while serial was bound to vt-1",
        )


if __name__ == "__main__":
    unittest.main()
