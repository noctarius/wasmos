import os
import re
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config

# Printed by the device manager as it starts, before it binds any driver and so
# before the serial driver exists.
EARLY = b"[device-manager] rule roots"
WORD = "typeahead_7c3e"
OUTPUT = re.compile(rb"(?:^|\n)" + WORD.encode() + rb"\r?\n")


class SerialTypeaheadTest(unittest.TestCase):
    """Keys typed before the serial driver starts reach the shell.

    Serial input reaches the vt only through the serial driver
    (src/drivers/tty/serial), which the device manager starts late in boot;
    until then a keystroke waits in the UART. A prompt can be on screen before
    the driver runs, because the vt repaints it around log lines, so typing at
    a prompt does not prove the driver is up.

    Regression: 2026-11-28-serial-typeahead-lost -- the 16550 driver's init
    wrote FCR 0xC7, clearing the receive FIFO, so whatever was typed before it
    started was discarded; only the bytes typed after it reached the shell
    ("cd /boot/apps" arrived as "pps"). With one CPU under WARP the driver
    starts after the first prompt, so the first command of a session was
    lost, as test_hello_go's "never echoed" on out/default (x86_64 WARP, one
    CPU) showed. The PL011 half (its init clears the pending receive
    interrupt) is covered where this battery runs on aarch64.
    """

    def test_a_command_typed_before_the_serial_driver_runs(self) -> None:
        session = QemuSession(default_config(), timeout_s=180, echo=True)
        try:
            session.start()
            if not session.expect_from(0, EARLY, timeout_s=120):
                self.fail(
                    f"the device manager never started\n--- tail ---\n{session.tail()}"
                )
            typed_at = session.mark()
            session.send(f"echo {WORD}")
            session.require_prompt()
            if not session.expect_from(typed_at, OUTPUT, timeout_s=60):
                self.fail(
                    "the command typed before the serial driver started was lost\n"
                    f"--- tail ---\n{session.tail()}"
                )
        finally:
            # A failed start or prompt has already stopped QEMU and reported why;
            # sending to it then would replace that report with "not running".
            if session.proc:
                session.send("halt")
            session.close()


if __name__ == "__main__":
    unittest.main()
