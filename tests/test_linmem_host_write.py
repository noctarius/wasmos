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


class LinmemHostWriteTests(unittest.TestCase):
    """A guest reads what a host call wrote to a page it had never touched.

    Regression: 2026-11-23-warp-r3-linmem-host-grow -- under WARP a host call
    that writes guest memory grows the module's live linear memory on the
    kernel side (warp_mem -> ensureLinearSize), committing new pages, but the
    ring-3 view of linear memory was only re-published at module start and on
    the guest's own grow request. The guest's next access to the grown range
    passed WARP's bounds check and faulted on a page that was committed but not
    mapped. It killed device-manager at boot in CI (critical unit, the whole
    boot halted) once its startup data crossed the initially mapped pages.

    linmem_host_write has the kernel write 96 KiB into its linear memory, past
    __heap_base where nothing has touched, and then reads the result. wasm3 has
    no such split and runs the same contract.
    """

    @classmethod
    def setUpClass(cls):
        cls.session = QemuSession(default_config(), timeout_s=240, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_guest_reads_what_a_host_call_wrote(self):
        start = self.session.send_command("spawn linmem_host_write", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(
                start, b"linmem-host-write: ok dispatch=", timeout_s=30
            ),
            "the guest did not read back what the host call wrote -- it faulted on "
            f"a page the kernel committed but never mapped for ring 3.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertNotIn(
            b"user-pf",
            bytes(self.session.buf[start:]),
            "the read of the host-written page faulted",
        )


if __name__ == "__main__":
    unittest.main()
