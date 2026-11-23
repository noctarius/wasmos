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


class PhysMapUnmappedTests(unittest.TestCase):
    """phys_map refuses physical memory nothing backs, instead of faulting.

    Regression: 2026-11-23-phys-map-unchecked -- wasmos_phys_map copied from
    the kernel's alias of the requested physical range without checking that
    the alias maps it, so a caller holding mmio.map that named an address
    beyond RAM panicked the kernel with a #PF in memcpy (wasm3). acpi-bus did
    exactly that when it read a stale XSDT entry.

    phys_map_unmapped asks for a page beyond the guest's RAM. Under wasm3, which
    reads through the alias, it must be told WASMOS_ERR_KERNEL_PHYS_UNMAPPED.
    WARP reads through a remapped scratch page, which reaches any physical
    address without a fault, so there a copy is within the contract. Either
    way the system must still be up afterwards.
    """

    # WASMOS_ERR_KERNEL_PHYS_UNMAPPED: domain kernel (19), code 12, negative.
    PHYS_UNMAPPED = -((19 << 16) | 12)

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

    def test_unbacked_physical_memory_is_refused(self):
        wasm3 = b"runtime=WASM3" in bytes(self.session.buf)
        start = self.session.send_command("spawn phys_map_unmapped", timeout_s=20)
        refused = f"phys-map-unmapped: refused code={self.PHYS_UNMAPPED}".encode()
        self.assertTrue(
            self.session.expect_from(start, b"phys-map-unmapped: ", timeout_s=30),
            "phys_map_unmapped reported nothing -- the kernel faulted on the read.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        if wasm3:
            self.assertTrue(
                self.session.expect_from(start, refused, timeout_s=10),
                "wasm3 did not refuse a physical page beyond RAM with "
                "kernel.PHYS_UNMAPPED.\n"
                f"--- tail ---\n{self.session.tail()}\n",
            )
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=30),
            "the system did not return to the prompt after the refused phys_map",
        )


if __name__ == "__main__":
    unittest.main()
