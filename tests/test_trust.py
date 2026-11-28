"""`trust` changes what the keyring trusts while the system runs.

Regression: 2026-11-28-start-anyway -- a package signed by a publisher this
system does not know could never be started, and a publisher's key could only
be trusted by placing its PEM file in /boot/system/keys before boot. macOS and
Windows let a user open an app from an unidentified developer anyway.

`trust allow <package>` sends the package's SHA-512 digest to the keyring, which
registers it with the kernel and appends it to /boot/system/keys/allow.lst: the
package then starts from bytecode with ipc.basic only. `trust add-key <pem>`
makes a publisher's key trusted and writes it beside the build's keys. Both need
keyring.admin, which the console shell holds; trust_noadmin is the same utility
without it, and the keyring refuses its requests.

hello_untrusted is hello_c signed with a key the kernel does not trust;
priv_rsa_untrusted is priv_probe signed with an RSA key the keyring does not
hold, whose public half the build installs as /boot/apps/rsa-untrusted.pem.

TrustSurvivesRestartTest boots twice on one ESP copy of its own: what the first
boot allowed and trusted is in force from the second boot's start.
"""

import os
import re
import shutil
import sys
import tempfile
import time
import unittest
from dataclasses import replace

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config

PROMPT = b"wamos> "
HELLO_OK = b"Hello from C"
REFUSED_HELLO = (
    b"[wasmos-app] refused, signed but verified by no trusted key: hello-untrusted"
)
ALLOWED_HELLO = (
    b"[wasmos-app] started anyway, verified by no trusted key but allowed: "
    b"hello-untrusted"
)
REFUSED_PROBE = (
    b"[wasmos-app] refused, signed but verified by no trusted key: priv-probe"
)
PUBLISHER_PROBE = (
    b"[pm] refused svc.control to priv-probe: only the system key grants it, "
    b"and the image is a publisher's"
)
ADDED_KEY = re.compile(rb"\[keyring\] trusted RSA key [0-9a-f]{32}: ([0-9a-f]{8}\.pem)")


class _SessionMixin:
    session: QemuSession

    def _log_since(self, start: int, syslog_start: int) -> bytes:
        # Refusals are kernel log, which may also or only reach the kernel-log
        # port; pumping moves pending serial output into buf.
        self.session._pump(0.25)
        return (
            bytes(self.session.buf[start:])
            + self.session.syslog_tail(1 << 20)[syslog_start:].encode()
        )

    def _run(self, command: str, *expect: bytes) -> bytes:
        """Run `command`, wait for every `expect` and the prompt; the log since."""
        syslog_start = len(self.session.syslog_tail(1 << 20))
        start = self.session.send_command(command, timeout_s=30)
        deadline = time.time() + 30
        log = self._log_since(start, syslog_start)
        while not all(e in log for e in expect) and time.time() < deadline:
            log = self._log_since(start, syslog_start)
        tail = f"--- tail ---\n{self.session.tail()}\n"
        for e in expect:
            self.assertIn(e, log, f"`{command}`: no {e!r}.\n{tail}")
        self.assertTrue(
            self.session.expect_from(start, PROMPT, timeout_s=30),
            f"no prompt after `{command}`.\n{tail}",
        )
        return self._log_since(start, syslog_start)


class TrustTest(_SessionMixin, unittest.TestCase):
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

    def test_an_allowed_package_starts_although_no_key_verifies_it(self):
        log = self._run("hello_untrusted", REFUSED_HELLO)
        self.assertNotIn(HELLO_OK, log)
        self._run(
            "trust allow /boot/apps/hello_untrusted.wap",
            b"trust: /boot/apps/hello_untrusted.wap may now start",
        )
        self._run("hello_untrusted", ALLOWED_HELLO, HELLO_OK)
        # The digest is recorded for the next boot, with the package it names.
        self._run("cat /boot/system/keys/allow.lst", b" /boot/apps/hello_untrusted.wap")

    def test_a_key_added_at_run_time_makes_its_packages_a_publishers(self):
        self._run("priv_rsa_untrusted", REFUSED_PROBE)
        log = self._run(
            "trust add-key /boot/apps/rsa-untrusted.pem",
            b"trust: packages signed with the key in /boot/apps/rsa-untrusted.pem now start",
        )
        added = ADDED_KEY.search(log)
        self.assertIsNotNone(
            added, f"the keyring did not log the key.\n{log[-2000:]!r}"
        )
        # A publisher's package starts; its svc.control is refused for that
        # reason and no other.
        self._run("priv_rsa_untrusted", PUBLISHER_PROBE)
        # The key is written beside the build's keys, for the next boot.
        self._run(
            f"cat /boot/system/keys/{added.group(1).decode()}",
            b"-----BEGIN PUBLIC KEY-----",
        )

    def test_a_sender_without_keyring_admin_changes_nothing(self):
        log = self._run(
            "trust_noadmin allow /boot/apps/hello_c.wap",
            b"was not allowed, the keyring refused it",
        )
        self.assertIn(b"NOT_AUTHORIZED", log)
        self.assertNotIn(b"may now start", log)


class TrustSurvivesRestartTest(_SessionMixin, unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="wasmos-trust-")
        esp = os.path.join(self.root, "esp")
        shutil.copytree(default_config().esp_dir, esp)
        # The two boots share this copy, so the first one's writes reach the
        # second; nothing else boots it.
        self.cfg = replace(default_config(), esp_dir=esp, isolate_esp=False)
        self.session = None

    def tearDown(self):
        if self.session:
            self.session.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def _boot(self) -> QemuSession:
        self.session = QemuSession(self.cfg, timeout_s=180, echo=True)
        self.session.start()
        self.session.require_prompt()
        return self.session

    def _halt(self) -> None:
        start = self.session.send_command("halt", timeout_s=30)
        self.session.expect_from(start, b"shutdown: complete", timeout_s=60)
        self.session.close()
        self.session = None

    def test_what_was_allowed_and_trusted_is_in_force_after_a_restart(self):
        self._boot()
        self._run("trust allow /boot/apps/hello_untrusted.wap", b"may now start")
        log = self._run("trust add-key /boot/apps/rsa-untrusted.pem", b"now start")
        added = ADDED_KEY.search(log)
        self.assertIsNotNone(
            added, f"the keyring did not log the key.\n{log[-2000:]!r}"
        )
        key_file = added.group(1).decode()
        self._halt()

        keys = os.path.join(self.cfg.esp_dir, "system", "keys")
        self.assertTrue(os.path.exists(os.path.join(keys, key_file)), os.listdir(keys))
        with open(os.path.join(keys, "allow.lst"), "rb") as f:
            self.assertIn(b" /boot/apps/hello_untrusted.wap\n", f.read())

        session = self._boot()
        boot = session.buf.decode("utf-8", "replace") + session.syslog_tail(1 << 20)
        self.assertIn("[keyring] 1 package(s) allowed to start anyway: allow.lst", boot)
        self.assertIn(f": {key_file}", boot)
        self.assertIn("[keyring] ready, 4 key(s) registered", boot)
        self._run("hello_untrusted", ALLOWED_HELLO, HELLO_OK)
        self._run("priv_rsa_untrusted", PUBLISHER_PROBE)
        self._halt()


if __name__ == "__main__":
    unittest.main()
