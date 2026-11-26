"""An app links Monocypher from the SDK and uses it.

The SDK ships libs/monocypher as libmonocypher.a with its headers in the
sysroot, for apps to link with -lmonocypher. crypto_smoke signs a message with
Ed25519, checks the signature and that an altered message fails it, and hashes
with BLAKE2b; it reports "crypto-smoke: ok" only when all of that held.
"""

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


class SdkMonocypherTest(unittest.TestCase):
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

    def test_an_app_signs_and_verifies_with_the_sdk_library(self):
        start = self.session.send_command("crypto_smoke", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(start, b"crypto-smoke: ok", timeout_s=30),
            f"crypto_smoke did not report ok.\n--- tail ---\n{self.session.tail()}\n",
        )


if __name__ == "__main__":
    unittest.main()
