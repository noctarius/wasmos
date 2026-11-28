"""The keyring registers the trusted package-signing keys at boot.

The keyring service (src/services/keyring) lists /boot/system/keys, reads each
PEM file and registers its Ed25519 key with the kernel; only then does it
register its name, which sysinit waits for before starting anything signed with
a publisher's key. The build installs its system key (system.pem) and its app
key (apps.pem) there, and signs the examples/ apps that ask only for app kinds
with the app key.

Regression: 2026-11-28-keyring -- the kernel trusted no key but the system key
compiled into it, so a package signed with any other key, the build's own app
key included, was refused.
"""

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

TRUSTED = re.compile(r"\[keyring\] trusted Ed25519 key ([0-9a-f]{32}): (\S+)")
HELLO_OK = b"Hello from C"


class KeyringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.session = QemuSession(default_config(), timeout_s=180, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _log(self) -> str:
        return self.session.buf.decode("utf-8", "replace") + self.session.syslog_tail(
            1 << 20
        )

    def test_the_build_keys_are_registered_at_boot(self):
        log = self._log()
        files = {name: key_id for key_id, name in TRUSTED.findall(log)}
        self.assertIn(
            "apps.pem", files, f"the app key was not registered\n{log[-3000:]}"
        )
        self.assertIn(
            "system.pem", files, f"the system key was not read\n{log[-3000:]}"
        )
        self.assertNotEqual(files["apps.pem"], files["system.pem"])
        self.assertIn("[keyring] ready, 2 key(s) registered", log)

    def test_an_app_signed_with_the_app_key_runs(self):
        # hello_c asks only for ipc.basic, so the build signs it with the app
        # key; without the keyring the kernel would refuse it.
        start = self.session.send_command("spawn /boot/apps/hello_c", timeout_s=30)
        self.assertTrue(
            self.session.expect_from(start, HELLO_OK, timeout_s=30),
            f"hello_c did not run\n--- tail ---\n{self.session.tail()}",
        )
        self.assertNotIn(
            "refused, signed but verified by no trusted key: hello-c", self._log()
        )


if __name__ == "__main__":
    unittest.main()
