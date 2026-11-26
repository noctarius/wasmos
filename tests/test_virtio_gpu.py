import os
import re
import sys
import tempfile
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config, guest_arch, requires_arch

# The virtio GPU on each architecture: aarch64's default display, and on
# x86_64 in place of the standard VGA (bochs) its machine brings otherwise.
_DISPLAY = {
    "aarch64": ("-device", "virtio-gpu-pci"),
    "x86_64": ("-vga", "none", "-device", "virtio-gpu-pci"),
}


class VirtioGpuBootTest(unittest.TestCase):
    """A boot whose display is a virtio GPU.

    edk2's virtio-gpu GOP is blit-only, so the loader allocates the scanout
    itself and hands it over as the framebuffer. The virtio_gpu driver attaches
    those pages to a device resource and presents what the compositor and the
    text console report drawing (DISPLAY_IPC_FLUSH_REQ). What QEMU actually shows
    is read back with a screendump, so the assertions do not rest on anything
    the system says about itself.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmpdir = tempfile.mkdtemp(prefix="wasmos-vgpu-")
        cfg = default_config()
        cfg.display_devices = _DISPLAY[guest_arch()]
        # screendump needs the display device, which -nographic removes;
        # display "none" keeps the run headless.
        cfg.nographic = False
        cfg.display = "none"
        cfg.enable_monitor = True
        cfg.monitor_socket = os.path.join(cls.tmpdir, "mon.sock")
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

    def test_driver_presents_what_the_writers_report(self) -> None:
        buf = bytes(self.session.buf)
        self.assertIn(b"[virtio-gpu] scanout 1280x800", buf)
        # The compositor and the text console each report their damage; the
        # periodic present only covers writers that do not.
        clients = buf.count(b"[virtio-gpu] presenting for client")
        self.assertGreaterEqual(
            clients,
            2,
            f"{clients} writer(s) flushed; expected the compositor and the text "
            f"console\n--- tail ---\n{self.session.tail()}",
        )

    def test_the_scanout_shows_the_desktop(self) -> None:
        path = os.path.join(self.tmpdir, "scanout.ppm")
        self.session.monitor.screendump(path, fmt="ppm")
        time.sleep(0.5)
        with open(path, "rb") as f:
            data = f.read()
        header, _, rest = data.partition(b"\n255\n")
        width, height = map(int, header.split()[1:3])
        self.assertEqual((width, height), (1280, 800))
        lit = sum(
            1
            for i in range(0, len(rest) - 2, 3 * 101)
            if rest[i] or rest[i + 1] or rest[i + 2]
        )
        # A presenter that never flushed leaves QEMU showing an all-black
        # resource; the desktop lights most of the sampled pixels.
        self.assertGreater(
            lit,
            len(rest) // (3 * 101) // 4,
            f"only {lit} sampled pixels are lit: nothing reached the scanout",
        )

    def test_loader_allocates_the_scanout(self) -> None:
        self.assertIn(
            b"[boot] blit-only GOP: scanout allocated by the loader",
            bytes(self.session.buf),
        )

    @requires_arch(
        "aarch64", reason="fw_cfg (QEMU0002) and its ramfb rule are QEMU virt's"
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


class VirtioGpuOwnershipTest(unittest.TestCase):
    """The GPU's PCI function belongs to virtio_gpu, the driver that programs it.

    Two rules match the virtio GPU -- virtio_gpu and the text console's
    framebuf -- and a PCI-rule spawn that carries capabilities makes its driver
    the function's owner, whose end quiesces the device. framebuf declares no
    register windows and the function has no I/O window, so it is spawned
    without capabilities and claims nothing; were it ever to claim the function,
    virtio_gpu's end would leave the device mastering into freed pages. Ending
    virtio_gpu must therefore quiesce the function, and its restart must bring
    the scanout back.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cfg.display_devices = _DISPLAY[guest_arch()]
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

    def test_ending_virtio_gpu_quiesces_its_function(self) -> None:
        # The unit's name comes from the supervisor's table, asked once the
        # system is idle. device-manager's boot line naming it shares the
        # console with every driver starting at the same moment, and another
        # driver's output can land in the middle of it.
        start = self.session.send_command("svc list", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=30),
            f"no prompt after svc list\n--- tail ---\n{self.session.tail()}",
        )
        listing = bytes(self.session.buf[start:])
        m = re.search(
            rb"^(virtio_gpu@pci:([0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f])) ",
            listing,
            re.MULTILINE,
        )
        self.assertIsNotNone(
            m,
            f"the supervisor holds no virtio_gpu unit\n--- svc list ---\n"
            f"{listing.decode(errors='replace')}",
        )
        unit, bdf = m.group(1).decode(), m.group(2).decode()
        mark = self.session.send_command(f"svc kill {unit}", timeout_s=20)
        self.assertTrue(
            self.session.expect_from(
                mark, f"[pci] quiesced {bdf}:".encode(), timeout_s=20
            ),
            f"ending {unit} did not quiesce {bdf}: another driver owns the "
            f"function\n--- tail ---\n{self.session.tail()}",
        )
        # The supervisor restarts it; the successor owns the function again
        # and gets its MSI-X vector.
        self.assertTrue(
            self.session.expect_from(
                mark, b"[virtio-gpu] scanout 1280x800", timeout_s=30
            ),
            f"virtio_gpu did not come back\n--- tail ---\n{self.session.tail()}",
        )


if __name__ == "__main__":
    unittest.main()
