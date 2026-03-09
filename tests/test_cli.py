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


class CliIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            cls.session.close()
            raise RuntimeError("CLI prompt not detected")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _cmd_expect(self, cmd: str, needle: bytes, timeout_s: int = 10) -> None:
        mark = self.session.mark()
        self.session.send(cmd)
        ok = self.session.expect_from(mark, needle, timeout_s=timeout_s)
        if not ok:
            self.fail(f"Expected output not found for '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")
        ok = self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s)
        if not ok:
            self.fail(f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n")

    def test_help_lists_commands(self):
        self._cmd_expect("help", b"commands:")

    def test_ps_lists_processes(self):
        self._cmd_expect("ps", b"pid")

    def test_ls_lists_root(self):
        self._cmd_expect("ls", b"apps")

    def test_cd_and_ls_apps(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd apps", b"/apps wamos>")
        self._cmd_expect("ls", b"hello_c.wasmosapp")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_cd_nested_services(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd system", b"/system wamos>")
        self._cmd_expect("cd services", b"/system/services wamos>")
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd /system/services", b"/system/services wamos>")
        self._cmd_expect("ls", b"cli.wasmosapp")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_cd_nested_drivers(self):
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd system", b"/system wamos>")
        self._cmd_expect("cd drivers", b"/system/drivers wamos>")
        self._cmd_expect("cd /", b"/ wamos>")
        self._cmd_expect("cd /system/drivers", b"/system/drivers wamos>")
        self._cmd_expect("ls", b"ata.wasmosapp")
        self._cmd_expect("cd /", b"/ wamos>")

    def test_cat_startup(self):
        self._cmd_expect("cat startup.nsh", b"BOOTX64.EFI")

if __name__ == "__main__":
    unittest.main()
