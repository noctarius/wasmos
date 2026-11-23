import os
import sys
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config, requires_arch


class _KernelLogPortChecks:
    """What any kernel-log port must hold: the boot from its first lines, every
    service line after it, and no shell output, which belongs to its slot and
    stays on COM1. Subclasses pick the port."""

    klog_port = ""
    session = None

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cfg.klog_port = cls.klog_port
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _syslog_contains(self, needle: str, timeout_s: float = 10.0) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if needle in self.session.syslog_tail(1 << 20):
                return True
            time.sleep(0.25)
        return False

    def _require(self, needle: str, why: str) -> None:
        self.assertTrue(
            self._syslog_contains(needle),
            f"{why}\n--- syslog ---\n{self.session.syslog_tail()}\n",
        )

    def test_the_port_holds_the_kernel_log_and_not_shell_output(self):
        self._require("[kernel] kmain", "the port does not start with the boot")
        self._require("[calculator] ready", "a service's log line is missing")

        start = self.session.send_command("echo shell_only_marker", timeout_s=20)
        if not self.session.expect_from(start, b"shell_only_marker", timeout_s=20):
            self.fail(
                f"the shell did not answer.\n--- tail ---\n{self.session.tail()}\n"
            )
        if not self.session.expect_from(start, b"wamos> ", timeout_s=20):
            self.fail(f"no prompt after echo.\n--- tail ---\n{self.session.tail()}\n")
        self.assertNotIn(
            "shell_only_marker",
            self.session.syslog_tail(1 << 20),
            "a shell's output reached the kernel-log port",
        )


class VirtioKernelLogPortTests(_KernelLogPortChecks, unittest.TestCase):
    """The default port: a virtio console that virtio_serial feeds from the kmsg
    ring. The driver starts well after boot, so "[kernel] kmain" on it proves
    the replay from the oldest retained byte, and the driver's own line names
    the device it found."""

    klog_port = "virtio"

    def test_the_driver_replays_the_ring_onto_the_console(self):
        self._require(
            "[virtio-serial] kernel log on virtio console",
            "virtio_serial did not come up on the console",
        )
        self.assertNotIn(
            "[virtio-serial] ",
            self.session.syslog_tail(1 << 20).replace(
                "[virtio-serial] kernel log on virtio console", ""
            ),
            "the replay reported kernel log lost or not retained",
        )


@requires_arch(
    "x86_64", reason="x86_64 hardware: the kernel log on a second 16550 (COM2)"
)
class Com2KernelLogPortTests(_KernelLogPortChecks, unittest.TestCase):
    """The x86_64 alternative: with log.serial=com2 the kernel writes the second
    UART itself, from an early-log replay onward, with no driver involved."""

    klog_port = "com2"

    def test_the_kernel_enables_com2(self):
        self._require(
            "[kernel] log.serial=com2: kernel log also on COM2",
            "the kernel did not enable COM2",
        )


if __name__ == "__main__":
    unittest.main()
