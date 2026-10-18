#!/usr/bin/env python3
"""A boot that cannot spawn sysinit PANICS, instead of hanging forever.

sysinit ships on the ESP rather than in the initfs, so init cannot spawn it
until device enumeration has mounted /boot. The spawn therefore RETRIES -- and
retried without any bound, so a boot where /boot never became readable asked
forever. One CI capture logged `[pm] spawn_path fs read failed:
/boot/system/services/sysinit.wap` **17,081 times across 14 minutes**, pegging a
CPU and reaching no prompt. That is the whole of `[BUG][P0]`: the guest never
dies, so there is no panic and no reset, and it never progresses, so whichever
battery happened to open a session first went red with an unrelated-looking
message.

The failure is induced by giving the guest an ESP with sysinit.wap removed,
which is the one condition that reproduces it deterministically. Everything else
about the boot is normal, so what is measured is the retry bound and nothing
else.

It panics rather than parking because sysinit is this system's pid 1: it reads
sysinit.rc and starts every service, so a boot that cannot spawn it has no
userspace and never will. Parking would leave a kernel running with nothing on
top -- alive enough to look like a hang, dead enough to be useless -- which is
the state this bug has been hiding in. Linux calls the same thing "Attempted to
kill init".

What is asserted is the bound AND the sentence. Stopping silently would turn a
14-minute hang into a shorter one; the point is that the boot NAMES what it
could not read, so the next occurrence is one line of triage rather than
twenty-five thousand lines of log.

Regression: 2026-10-18-init-retried-the-sysinit-spawn-forever
"""

import os
import re
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from scripts.qemu_test_framework import QemuSession, default_config

SYSINIT = os.path.join("system", "services", "sysinit.wap")
# Matches INIT_SYSINIT_MAX_ATTEMPTS in src/kernel/kernel_init_runtime.c. Not
# imported from anywhere -- it is a C define -- so a change to it fails here and
# the two are reconciled deliberately rather than silently drifting.
MAX_ATTEMPTS = 512


class InitSysinitGiveUpTest(unittest.TestCase):
    """An ESP without sysinit, which is the only way to reach the bound."""

    session: QemuSession | None = None
    tmpdir: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.tmpdir = tempfile.mkdtemp(prefix="wasmos-nosysinit-")
        esp = os.path.join(cls.tmpdir, "esp")
        # A private copy: the real ESP is shared with every other target, and a
        # test that deleted from it would break the rest of the suite.
        shutil.copytree(cfg.esp_dir, esp)
        os.remove(os.path.join(esp, SYSINIT))
        cfg.esp_dir = esp
        cls.session = QemuSession(cfg, timeout_s=300, echo=False)
        cls.session.start()
        # No require_prompt: this boot is NOT expected to reach one. That is the
        # condition under test, not a failure of the harness.
        cls.session.expect(b"init_no_sysinit", timeout_s=240)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls.tmpdir:
            shutil.rmtree(cls.tmpdir, ignore_errors=True)
            cls.tmpdir = None

    def test_the_boot_panics_rather_than_retrying_forever(self) -> None:
        assert self.session is not None
        self.assertIn(
            b"init_no_sysinit",
            self.session.buf,
            "init never stopped retrying the sysinit spawn, so this boot is the "
            "unbounded hang the bound exists to end\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )

    def test_it_is_a_panic_and_not_a_quiet_park(self) -> None:
        """A kernel with no userspace should say so, not idle politely.

        Parking is what made this bug expensive: the guest stayed alive, so
        there was no panic to find and no reset to detect, and the only symptom
        was whichever test happened to open the next session.
        """
        assert self.session is not None
        self.assertIn(
            b"KERNEL PANIC",
            self.session.buf,
            "the boot stopped without panicking, so a system with no userspace "
            "still looks like a slow one\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )

    def test_it_says_what_it_could_not_read_and_where_to_look(self) -> None:
        """The sentence is the deliverable, not just the stopping.

        A triage that only learns "the boot stopped" is the position this bug
        has been in all along.
        """
        assert self.session is not None
        self.assertIn(
            b"sysinit starts every service",
            self.session.buf,
            "the give-up does not say what went wrong, so it replaces a long "
            "mystery with a short one\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )

    def test_the_retries_are_bounded_at_the_declared_limit(self) -> None:
        """The count, because a bound nobody checks drifts.

        PM logs one line per failed read, so counting them counts the retries.
        Asserted as a ceiling rather than an equality: the boot may fail to read
        other ESP modules too, and those lines are not sysinit's.
        """
        assert self.session is not None
        attempts = len(
            re.findall(
                rb"spawn_path fs read failed: /boot/system/services/sysinit\.wap",
                self.session.buf,
            )
        )
        self.assertGreater(attempts, 0, "the read never failed, so nothing was bounded")
        self.assertLessEqual(
            attempts,
            MAX_ATTEMPTS + 1,
            f"the sysinit spawn was retried {attempts} times against a declared "
            f"bound of {MAX_ATTEMPTS}; before the bound existed one CI capture "
            "reached 17081\n",
        )


if __name__ == "__main__":
    unittest.main()
