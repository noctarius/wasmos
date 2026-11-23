#!/usr/bin/env python3
"""Both 16550 UARTs get a driver of their own, and the console keeps its input.

Regression: 2026-11-17-serial-hardwired-to-com1 -- the `serial` driver served
COM1 whatever device it was spawned for. Bound to both UARTs, the instance the
device manager spawned for COM2 (which ACPI lists first) programmed COM1's
registers, asked for COM1's IRQ and was refused it, and took the "sin" name the
vt subscribes to; the instance for COM1 then died before it was ready, every
time. Before the device manager learned to give up on such a device, the boot
respawned it until the session timed out; after, the boot finished with console
input routed to a driver polling a port it was never granted, so the prompt
appeared and took no command.

The shipped rule binds COM1 only (`scripts/system/devmgr/rules/default.x86_64.rules`),
so this boots a private copy of the ESP whose rule matches every 16550, which is
the configuration a machine with a second serial port needs.
"""

import os
import shutil
import sys
import tempfile
import unittest
from dataclasses import replace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from scripts.qemu_test_framework import QemuSession, default_config, requires_arch

RULES = os.path.join("system", "devmgr", "rules", "default.rules")
COM1_ONLY = 'ATTR{io}=="0x3F8", RUN+="system/drivers/uart16550.wap"'
EVERY_UART = 'RUN+="system/drivers/uart16550.wap"'


def _esp_binding_every_uart(esp_dir: str, into: str) -> str:
    """A copy of `esp_dir` whose boot rules bind `serial` to every 16550."""
    copy = os.path.join(into, "esp")
    shutil.copytree(esp_dir, copy)
    path = os.path.join(copy, RULES)
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    if COM1_ONLY not in text:
        raise AssertionError(
            f"{RULES} no longer carries the COM1-only serial rule this test widens"
        )
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text.replace(COM1_ONLY, EVERY_UART))
    return copy


@requires_arch("x86_64", reason="x86_64 hardware: a second 16550 COM port")
class SerialSecondUartTest(unittest.TestCase):
    session: QemuSession | None = None
    work_dir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.work_dir = tempfile.mkdtemp(prefix="wasmos-second-uart-")
        esp = _esp_binding_every_uart(cfg.esp_dir, cls.work_dir)
        # The second 16550 this test is about. Nothing else attaches one: the
        # kernel log goes to the virtio console, so COM2 has no other user.
        cls.session = QemuSession(
            replace(
                cfg,
                esp_dir=esp,
                klog_port="virtio",
                extra_args=(*cfg.extra_args, "-serial", "null"),
            ),
            timeout_s=240,
        )
        cls.session.start()
        cls.session.require_prompt(timeout_s=200)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.work_dir:
            shutil.rmtree(cls.work_dir, ignore_errors=True)
            cls.work_dir = None

    def test_each_uart_is_served_on_its_own_port_and_irq(self) -> None:
        assert self.session is not None
        for line in (
            b"[serial] driver ready io=0x3f8 irq=4 (IRQ-driven)",
            b"[serial] driver ready io=0x2f8 irq=3 (IRQ-driven)",
        ):
            self.assertTrue(
                self.session.expect(line, timeout_s=60),
                f"no {line!r}: the instance for that UART did not come up on "
                "the port and IRQ it was spawned for",
            )

    def test_no_instance_is_given_up_on(self) -> None:
        assert self.session is not None
        self.assertFalse(
            b"gave up on system/drivers/uart16550.wap" in self.session.buf,
            "the device manager gave up on a serial instance, so one UART went "
            "undriven",
        )

    def test_the_console_still_takes_input(self) -> None:
        """The vt reads the console through "sin"; a second instance must not
        take that name, or the prompt appears and accepts nothing."""
        assert self.session is not None
        start = self.session.send_command("echo second-uart-ok", timeout_s=60)
        self.assertTrue(
            self.session.expect_from(start, b"second-uart-ok\r\n", timeout_s=60),
            "the console did not echo a command: its input is not reaching the " "CLI",
        )


if __name__ == "__main__":
    unittest.main()
