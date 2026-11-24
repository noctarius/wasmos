import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config, requires_arch


@requires_arch(
    "aarch64",
    reason="QEMU virt with virtio-gpu-pci and fw_cfg; x86_64 keeps bochs",
)
class VirtioGpuBootTest(unittest.TestCase):
    """A boot whose display is a virtio GPU.

    edk2's virtio-gpu GOP is blit-only, so the loader allocates the scanout
    itself and hands it over as the framebuffer.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cfg.display_devices = ("-device", "virtio-gpu-pci")
        cls.session = QemuSession(cfg, timeout_s=180, echo=True)
        cls.session.start()
        cls.session.require_prompt()
        if not cls.session.settle():
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(
                f"system never went idle after boot\n--- tail ---\n{tail}\n"
            )

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_loader_allocates_the_scanout(self) -> None:
        self.assertIn(
            b"[boot] blit-only GOP: scanout allocated by the loader",
            bytes(self.session.buf),
        )

    def test_ramfb_driver_declines_a_framebuffer_that_is_not_a_ramfb(self) -> None:
        """Regression: 2026-11-24-ramfb-claims-foreign-framebuffer.

        fw_cfg (ACPI QEMU0002) is on every QEMU virt machine, so the ramfb
        driver is spawned whenever a framebuffer exists. With the loader's
        virtio-gpu scanout it mapped and CLEARED that framebuffer, then failed
        for want of etc/ramfb and was retried eight times -- each retry wiping
        the console of the driver the framebuffer did belong to.
        """
        buf = bytes(self.session.buf)
        loads = buf.count(b"[native-driver] loading fbramfb")
        self.assertEqual(
            loads,
            1,
            f"the ramfb driver was loaded {loads} times\n"
            f"--- tail ---\n{self.session.tail()}",
        )
        self.assertIn(b"[framebuffer-ramfb] the framebuffer is not this", buf)


if __name__ == "__main__":
    unittest.main()
