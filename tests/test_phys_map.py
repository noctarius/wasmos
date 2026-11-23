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


class PhysMapTests(unittest.TestCase):
    """wasmos_phys_map refuses a caller without mmio.map, and a source range the
    kernel's alias of physical memory does not map, on both runtimes."""

    # Packed kernel-domain (19) codes, negative: NOT_AUTHORIZED is 4,
    # PHYS_UNMAPPED is 12.
    NOT_AUTHORIZED = -((19 << 16) | 4)
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

    def _spawn_and_expect(self, app, tag, code, failure):
        start = self.session.send_command(f"spawn {app}", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(start, f"{tag}: ".encode(), timeout_s=30),
            f"{app} reported nothing -- the kernel faulted on the read.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertTrue(
            self.session.expect_from(
                start, f"{tag}: refused code={code}".encode(), timeout_s=10
            ),
            f"{failure}\n--- tail ---\n{self.session.tail()}\n",
        )
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=30),
            f"the system did not return to the prompt after {app}",
        )

    def test_caller_without_mmio_map_is_refused(self):
        """Regression: 2026-11-23-warp-phys-map-unauthorized -- WARP's
        wasmos_phys_map never checked the mmio.map capability, so any WARP
        guest importing it could copy arbitrary physical memory (kernel frames,
        other processes' pages) into its linear memory. phys_map_denied holds
        only ipc.basic and asks for a page of RAM; it must be told
        kernel.NOT_AUTHORIZED, not handed the page."""
        self._spawn_and_expect(
            "phys_map_denied",
            "phys-map-denied",
            self.NOT_AUTHORIZED,
            "phys_map copied physical memory for a caller without mmio.map "
            "instead of refusing it with kernel.NOT_AUTHORIZED.",
        )

    def test_unbacked_physical_memory_is_refused(self):
        """Regression: 2026-11-23-phys-map-unchecked -- wasmos_phys_map copied
        from the kernel's alias of the requested physical range without
        checking that the alias maps it, so a caller holding mmio.map that
        named an address beyond RAM panicked the kernel with a #PF in memcpy
        (wasm3). acpi-bus did exactly that when it read a stale XSDT entry.
        WARP read through a scratch page remapped onto any physical frame,
        including device memory, and copied whole frames from the wrong offset
        for an unaligned address.

        phys_map_unmapped asks for the page at 4 GiB, outside the alias window
        on both architectures (512 MiB on x86_64, 4 GiB on aarch64). Both
        runtimes must refuse it with kernel.PHYS_UNMAPPED and stay up."""
        self._spawn_and_expect(
            "phys_map_unmapped",
            "phys-map-unmapped",
            self.PHYS_UNMAPPED,
            "phys_map did not refuse a physical page beyond the alias window "
            "with kernel.PHYS_UNMAPPED.",
        )


if __name__ == "__main__":
    unittest.main()
