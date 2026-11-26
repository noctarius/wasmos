"""Only the bus services and storage managers may publish device inventory.

Regression: 2026-11-26-devmgr-inventory-unauthenticated -- device-manager's
inventory endpoint ("devmgr.inv") is discoverable by name, and every
DEVMGR_PUBLISH_* record it received went into the inventory without a check of
who sent it. The inventory decides which drivers device-manager spawns and what
it grants them -- register windows, an interrupt, DMA -- so any process could
publish a forged PCI function, ACPI device, block device or volume and have a
driver spawned over hardware of its choosing.

inv_forge holds only ipc.basic and publishes a block device, "block:forged:0",
exactly as a block driver does. Device-manager must refuse it and say so.
"""

import os
import re
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

ACCEPTED = b"block add id=block:forged:0"
REFUSED = re.compile(
    rb"\[device-manager\] refused inventory message [^\r\n]* from a sender "
    rb"without inventory\.storage"
)


class DevmgrInventoryAuthTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.session = QemuSession(default_config(), timeout_s=120, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _log(self) -> bytes:
        # device-manager's lines are console output, which may also or only
        # reach the kernel-log port. Pumping the session moves pending serial
        # output into buf; syslog_tail reads its own file.
        self.session._pump(0.25)
        return bytes(self.session.buf) + self.session.syslog_tail(1 << 20).encode()

    def test_an_app_cannot_publish_a_block_device(self):
        start = self.session.send_command("inv_forge", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(
                start, b"[inv-forge] published block:forged:0", timeout_s=30
            ),
            f"the forger did not run.\n--- tail ---\n{self.session.tail()}\n",
        )
        deadline = time.time() + 10
        while time.time() < deadline:
            log = self._log()
            if ACCEPTED in log or REFUSED.search(log):
                break
        log = self._log()
        self.assertFalse(
            ACCEPTED in log,
            f"device-manager added the forged block device.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertTrue(
            REFUSED.search(log),
            f"device-manager did not report refusing the record.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )


if __name__ == "__main__":
    unittest.main()
