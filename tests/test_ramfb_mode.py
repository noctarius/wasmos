import os
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

from qemu_test_framework import QemuSession, default_config, requires_arch

# The mode the bootloader picks on ramfb: the largest it offers within 1280x800.
# edk2 allocates the buffer for exactly this mode, so every runtime mode must
# fit in it.
BOOT_MODE = (1024, 768)


def ppm_size(path):
    """(width, height) from the header of QEMU's binary PPM screendump."""
    with open(path, "rb") as fh:
        data = fh.read(64)
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM")
    fields = data[2:].split()
    return int(fields[0]), int(fields[1])


@requires_arch(
    "aarch64",
    reason="ramfb is the aarch64 display; x86_64 changes modes through the Bochs VBE "
    "adapter (framebuffer_pci)",
)
class RamfbModeTests(unittest.TestCase):
    """A display mode requested from the compositor reaches QEMU's ramfb.

    The fixture asks the compositor (GFX_IPC_SET_DISPLAY_MODE), the compositor
    asks the framebuffer driver (FBTEXT SET_RESOLUTION), and framebuffer_ramfb
    rewrites the fw_cfg file etc/ramfb through fw_cfg DMA. What QEMU then scans
    out is read back with a screendump: the size of that image is QEMU's own
    answer, not anything the system reports about itself. The compositor's
    GET_DISPLAY_INFO must agree with it, which holds only when the driver also
    updated the kernel's framebuffer info the compositor remaps from.
    """

    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.mkdtemp(prefix="wasmos-ramfb-")
        cfg = default_config()
        # ramfb rather than the architecture's default display (virtio-gpu),
        # which does not change modes at runtime.
        cfg.display_devices = ("-device", "ramfb")
        # screendump needs the display device, which -nographic removes;
        # display "none" keeps the run headless.
        cfg.nographic = False
        cfg.display = "none"
        cfg.enable_monitor = True
        cfg.monitor_socket = os.path.join(cls.tmpdir, "mon.sock")
        cls.session = QemuSession(cfg, timeout_s=180, echo=True)
        cls.session.start()
        cls.session.require_prompt()

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.close()

    def _scanout(self, name):
        path = os.path.join(self.tmpdir, f"{name}.ppm")
        self.session.monitor.screendump(path, fmt="ppm")
        return ppm_size(path)

    def _wait_for_scanout(self, name, expected, timeout_s=5.0):
        """The scanout size, re-dumped until it equals `expected` or time runs
        out; QEMU resizes its surface on its own display refresh."""
        deadline = time.monotonic() + timeout_s
        size = self._scanout(name)
        while size != expected and time.monotonic() < deadline:
            time.sleep(0.25)
            size = self._scanout(name)
        return size

    def _set_mode(self, width, height):
        start = self.session.send_command(
            f"spawn gfx_mode {width} {height}", timeout_s=20
        )
        self.assertTrue(
            self.session.expect_from(start, b"gfx-mode: display ", timeout_s=30),
            f"gfx_mode {width} {height} got no answer from the compositor.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=30),
            "the system did not return to the prompt after gfx_mode",
        )
        return start

    def test_a_mode_change_reaches_the_scanout_and_back(self):
        self.assertEqual(
            self._wait_for_scanout("boot", BOOT_MODE),
            BOOT_MODE,
            "QEMU does not scan out the boot mode the bootloader picked",
        )

        start = self._set_mode(640, 480)
        self.assertTrue(
            self.session.expect_from(
                start, b"gfx-mode: set status=0 640x480", timeout_s=5
            ),
            "the compositor did not apply 640x480, or reported another geometry.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertTrue(
            self.session.expect_from(start, b"gfx-mode: display 640x480", timeout_s=5),
            "after the change the compositor reports another display size.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertEqual(
            self._wait_for_scanout("small", (640, 480)),
            (640, 480),
            "the ramfb scanout did not change to 640x480: etc/ramfb was not rewritten",
        )

        start = self._set_mode(*BOOT_MODE)
        self.assertTrue(
            self.session.expect_from(
                start, b"gfx-mode: set status=0 1024x768", timeout_s=5
            ),
            f"the compositor did not return to 1024x768.\n--- tail ---\n{self.session.tail()}\n",
        )
        self.assertEqual(
            self._wait_for_scanout("restored", BOOT_MODE),
            BOOT_MODE,
            "the ramfb scanout did not return to 1024x768",
        )

    def test_a_mode_larger_than_the_boot_framebuffer_is_refused(self):
        start = self._set_mode(1280, 800)
        self.assertFalse(
            self.session.expect_from(start, b"gfx-mode: set status=0", timeout_s=2),
            "a mode that does not fit the boot framebuffer was applied",
        )
        self.assertTrue(
            self.session.expect_from(start, b"gfx-mode: display 1024x768", timeout_s=5),
            "a refused mode changed what the compositor reports.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertEqual(
            self._scanout("refused"),
            BOOT_MODE,
            "a refused mode changed the ramfb scanout",
        )


if __name__ == "__main__":
    unittest.main()
