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
    """wasmos_phys_map maps firmware ACPI table memory into the caller's
    linear memory, for a caller holding mmio.map, and refuses everything else,
    on both runtimes."""

    # Packed kernel-domain (19) codes, negative: NOT_AUTHORIZED is 4,
    # PHYS_NOT_FIRMWARE is 12.
    NOT_AUTHORIZED = -((19 << 16) | 4)
    PHYS_NOT_FIRMWARE = -((19 << 16) | 12)

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

    def _spawn(self, app, tag):
        start = self.session.send_command(f"spawn {app}", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(start, f"{tag}: ".encode(), timeout_s=30),
            f"{app} reported nothing -- the kernel faulted on the request.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        return start

    def _expect(self, start, line, failure):
        self.assertTrue(
            self.session.expect_from(start, line.encode(), timeout_s=10),
            f"{failure}\n--- tail ---\n{self.session.tail()}\n",
        )

    def _expect_prompt(self, start, app):
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
        start = self._spawn("phys_map_denied", "phys-map-denied")
        self._expect(
            start,
            f"phys-map-denied: refused code={self.NOT_AUTHORIZED}",
            "phys_map served a caller without mmio.map instead of refusing it "
            "with kernel.NOT_AUTHORIZED.",
        )
        self._expect_prompt(start, "phys_map_denied")

    def test_memory_that_is_not_a_firmware_table_is_refused(self):
        """Regression: 2026-11-23-phys-map-ram -- with mmio.map, wasmos_phys_map
        handed out any physical page the kernel could reach: system RAM, and
        with it kernel frames and other processes' pages, on both runtimes
        (wasm3 through its alias of physical memory, WARP through a scratch
        page remapped onto any frame). The capability exists to read firmware
        tables, so RAM must be refused like a page nothing backs.

        Regression: 2026-11-23-phys-map-unchecked -- the unbacked page at 4 GiB
        once panicked the kernel with a #PF in memcpy (wasm3); acpi-bus did
        exactly that when it read a stale XSDT entry."""
        start = self._spawn("phys_map_refused", "phys-map-refused")
        for label in ("ram", "unbacked"):
            self._expect(
                start,
                f"phys-map-refused: {label} refused code={self.PHYS_NOT_FIRMWARE}",
                f"phys_map did not refuse the {label} page with "
                "kernel.PHYS_NOT_FIRMWARE.",
            )
        self._expect_prompt(start, "phys_map_refused")

    def test_a_firmware_table_is_mapped_and_survives_its_holders_exit(self):
        """phys_map_acpi maps the root description table the kernel's RSDP
        names, reads its signature through the window, asks again for a range
        inside it, and exits with the window mapped. The second request must
        resolve to the first window rather than map again, and reaping the
        process must leave the firmware frames alone: under WARP the window
        sits in the linear-memory slot, whose decommit frees every frame still
        mapped there. Three rounds show the system survives each reap."""
        for _ in range(3):
            start = self._spawn("phys_map_acpi", "phys-map-acpi")
            self.assertTrue(
                self.session.expect_from(
                    start, b"phys-map-acpi: XSDT again=same", timeout_s=10
                )
                or self.session.expect_from(
                    start, b"phys-map-acpi: RSDT again=same", timeout_s=1
                ),
                "phys_map did not map the firmware's root description table, "
                "or mapped a range inside it a second time.\n"
                f"--- tail ---\n{self.session.tail()}\n",
            )
            self._expect_prompt(start, "phys_map_acpi")
        self.assertIn(
            b"[acpi-bus] DSDT ",
            bytes(self.session.buf),
            "acpi-bus did not map the DSDT at boot",
        )


if __name__ == "__main__":
    unittest.main()
