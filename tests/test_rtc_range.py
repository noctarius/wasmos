#!/usr/bin/env python3
"""A clock outside the years the rtc protocol carries is refused, not reported.

The rtc service answers in the range validateTime() accepts (1970-2099,
src/drivers/rtc/rtc_time.ts). The guest boots with its hardware clock in 2100,
so `date` must fail rather than print a year the protocol does not carry.

Regression: 2026-11-24-efi-rtc-unvalidated-read -- the firmware-clock backend
(Kconfig WASMOS_RTC_EFI) passed whatever GetTime returned on as a successful
read. UEFI's EFI_TIME carries years 1900-9999, so a clock in 2100 answered
2100-01-02 where the PL031 backend answered rtc.OUT_OF_RANGE: the same clock
read differently depending on the Kconfig choice.

aarch64 only: both of its clock choices read the PL031, whose 32-bit count
reaches 2106. The x86_64 CMOS keeps two year digits, so a 2100 there is read
as 2000 and the case does not arise.
"""

import os
import re
import sys
import unittest
from dataclasses import replace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from scripts.qemu_test_framework import QemuSession, default_config, requires_arch


@requires_arch("aarch64", reason="the x86_64 CMOS cannot hold a year past 2099")
class RtcRangeTests(unittest.TestCase):
    session = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.session = QemuSession(
            replace(
                cfg, extra_args=(*cfg.extra_args, "-rtc", "base=2100-01-02T03:04:05")
            ),
            timeout_s=120,
            echo=True,
        )
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_date_refuses_a_year_past_2099(self):
        start = self.session.send_command("date", timeout_s=20)
        if not self.session.expect_from(start, b"wamos> ", timeout_s=20):
            self.fail(f"no prompt after date.\n--- tail ---\n{self.session.tail()}\n")
        out = self.session.buf[start:].decode("utf-8", errors="replace")
        self.assertIsNone(re.search(r"\b2100-\d{2}-\d{2}", out), out)
        self.assertIn("date: read failed", out)


if __name__ == "__main__":
    unittest.main()
