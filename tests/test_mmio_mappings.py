"""Device-register pages are mapped uncached.

The kernel maps the LAPIC and IOAPIC register pages into the shared higher
half. A device register must never sit behind the cache: a read can be served
from a stale line instead of the device, and a write can sit in the cache
instead of reaching it. The check reads the live page tables from outside the
guest through QEMU's `info tlb`, whose attribute column is
`X G P D A C T U W` (NX, global, large page, dirty, accessed, PCD, PWT, user,
write); strong uncacheable (UC) under the default PAT is PCD and PWT together.
"""

import unittest

from scripts.qemu_test_framework import QemuSession, default_config

# Kernel VAs of the device-register windows (lapic.c LAPIC_VIRT_BASE,
# ioapic.c IOAPIC_VIRT_BASE).
DEVICE_PAGES = {
    "lapic": 0xFFFFFFFF800FE000,
    "ioapic": 0xFFFFFFFF800FF000,
}

# Column positions in QEMU's x86 `info tlb` attribute string.
PCD_COLUMN = 5
PWT_COLUMN = 6


def tlb_attributes(tlb: str) -> dict[int, str]:
    """Map virtual page address -> attribute string from `info tlb` output."""
    attrs = {}
    for line in tlb.splitlines():
        parts = line.split()
        if len(parts) != 3 or not parts[0].endswith(":"):
            continue
        try:
            attrs[int(parts[0][:-1], 16)] = parts[2]
        except ValueError:
            continue
    return attrs


class MmioMappingTest(unittest.TestCase):
    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.session = QemuSession(default_config(), timeout_s=180)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=150):
            diagnosis = cls.session.boot_diagnosis()
            cls.session.close()
            raise RuntimeError(diagnosis)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_device_register_pages_are_uncached(self) -> None:
        """Regression: 2026-11-21-mmio-mapped-cacheable -- lapic.c and ioapic.c
        asked for cache-disable by passing the raw PTE bit PCD to
        paging_map_4k, which takes MEM_REGION_FLAG_* and silently dropped it,
        so both register pages were mapped write-back. QEMU's TCG ignores
        cacheability, so nothing failed under emulation; on hardware or under a
        hypervisor a cached device register returns stale reads and holds
        writes back from the device."""
        assert self.session is not None
        if self.session.monitor is None:
            self.skipTest("QEMU monitor disabled (WASMOS_QEMU_MONITOR=0)")
        attrs = tlb_attributes(self.session.monitor.hmp("info tlb"))
        for name, va in DEVICE_PAGES.items():
            with self.subTest(page=name):
                self.assertIn(va, attrs, f"{name} page {va:#x} is not mapped")
                flags = attrs[va]
                self.assertEqual(
                    (flags[PCD_COLUMN], flags[PWT_COLUMN]),
                    ("C", "T"),
                    f"{name} page {va:#x} is mapped cacheable: attributes {flags!r}",
                )


if __name__ == "__main__":
    unittest.main()
