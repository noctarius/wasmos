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

from qemu_test_framework import QemuSession, default_config, guest_arch

# The text renderer's cell (src/drivers/gpu/framebuffer/fbtext_internal.h).
CELL_W = 8
CELL_H = 16
# A channel above this is ink; a text slot's background is black.
INK_LEVEL = 0x40
# Offered by both mode-setting drivers (framebuffer_pci on x86_64,
# framebuffer_ramfb on aarch64), and smaller than either boot mode.
SMALL_MODE = (640, 480)


def read_ppm(path):
    """(width, height, pixels) of QEMU's binary PPM screendump, maxval 255."""
    with open(path, "rb") as fh:
        data = fh.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM")
    fields = data[2:].split(maxsplit=3)
    width, height = int(fields[0]), int(fields[1])
    return width, height, data[len(data) - width * height * 3 :]


def text_rows(pixels, width, height):
    """Per text row of the frame: (any ink in the row, ink in its first cell)."""

    def inked(x0, x1, y0, y1):
        for y in range(y0, y1):
            row = pixels[(y * width + x0) * 3 : (y * width + x1) * 3]
            if any(channel > INK_LEVEL for channel in row):
                return True
        return False

    return [
        (inked(0, width, y, y + CELL_H), inked(0, CELL_W, y, y + CELL_H))
        for y in range(0, height - CELL_H + 1, CELL_H)
    ]


class VtModeRegridTests(unittest.TestCase):
    """A text slot follows a display mode change.

    The compositor asks the framebuffer driver for a new mode (FBTEXT
    SET_RESOLUTION); the driver then tells the vt (FBTEXT_IPC_GEOMETRY_CHANGED),
    which re-lays its slots out for the new text grid. The driver renders a blit
    at its own column count, so a slot laid out for any other grid reaches the
    screen sheared. What QEMU scans out is read back with a screendump.
    """

    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.mkdtemp(prefix="wasmos-vtregrid-")
        cfg = default_config()
        if guest_arch() == "aarch64":
            # ramfb rather than aarch64's default display (virtio-gpu), which
            # does not change modes at runtime.
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

    def _shot(self, name):
        path = os.path.join(self.tmpdir, f"{name}.ppm")
        self.session.monitor.screendump(path, fmt="ppm")
        return read_ppm(path)

    def _command(self, command, expect=b"wamos> "):
        start = self.session.send_command(command, timeout_s=20)
        self.assertTrue(
            self.session.expect_from(start, expect, timeout_s=30),
            f"no {expect!r} after {command!r}.\n--- tail ---\n{self.session.tail()}\n",
        )
        return start

    def _set_mode(self, width, height):
        start = self._command(f"spawn gfx_mode {width} {height}")
        self.assertTrue(
            self.session.expect_from(
                start, f"gfx-mode: set status=0 {width}x{height}".encode(), timeout_s=30
            ),
            f"the compositor did not apply {width}x{height}.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        # gfx_mode's last line follows one more compositor round-trip, after the
        # shell has already printed its prompt; typed before it, the next
        # command's echo is split by it and never matches.
        self.assertTrue(
            self.session.expect_from(
                start, f"gfx-mode: display {width}x{height}".encode(), timeout_s=30
            ),
            f"gfx_mode did not report the new display size.\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )

    # Regression: 2026-11-24-vt-keeps-boot-grid-after-mode-change -- the vt asked
    # for the text geometry only when the framebuffer driver was bound, so after a
    # resize it kept laying slots out on the boot grid. The driver renders a blit
    # at its own column count, so every row after the first was sheared across
    # the screen and a text slot was unreadable until the next reboot.
    def test_a_text_slot_is_laid_out_for_the_new_mode(self):
        boot_mode = self._shot("boot")[:2]
        self._set_mode(*SMALL_MODE)
        # vt-2 has no shell yet: binding serial to it spawns one, so its banner,
        # the probe and the prompts are all written after the resize. The last
        # three rows are the shell's prompts: adjacent, each starting in the
        # first column. A slot laid out for a wider grid shears them away from
        # it or, when the old width is a multiple of the new one (x86_64 boots
        # at 160 columns), spreads each over several rows.
        self._command("tty -s 2", expect=b"WAMOS CLI")
        self._command("echo regrid_probe", expect=b"regrid_probe")
        self._command("tty 2")
        # The repaint follows the prompt; re-dump until it is complete and laid
        # out, or time runs out.
        inked, sheared, spread, scanout = [], [], False, None
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            width, height, pixels = self._shot("vt2")
            scanout = (width, height)
            if scanout == SMALL_MODE:
                rows = text_rows(pixels, width, height)
                inked = [i for i, (ink, _) in enumerate(rows) if ink]
                prompts = inked[-3:]
                sheared = [i for i in prompts if not rows[i][1]]
                spread = prompts != list(range(inked[-1] - 2, inked[-1] + 1))
                if len(inked) >= 6 and not sheared and not spread:
                    break
            time.sleep(0.25)
        # Regression: 2026-11-24-bga-mode-set-refused -- framebuffer_pci held no
        # io.port capability, so every Bochs VBE register write was refused and
        # ignored: the mode change was reported as applied while QEMU kept
        # scanning out the boot mode, and the desktop and text were drawn at a
        # stride the scanout did not use.
        self.assertEqual(
            scanout,
            SMALL_MODE,
            "the scanout did not change to 640x480: the adapter was not reprogrammed",
        )
        self.assertGreaterEqual(
            len(inked), 6, f"vt-2 is not on screen at 640x480 (text rows {inked})"
        )
        self.assertEqual(
            sheared,
            [],
            "the prompts on vt-2 do not start in the first column: the slot is laid "
            f"out for another grid (inked rows {inked}).\n--- tail ---\n"
            f"{self.session.tail()}\n",
        )
        self.assertFalse(
            spread,
            "the prompts on vt-2 are not on adjacent rows: the slot is laid out for "
            f"a wider grid (inked rows {inked}).\n--- tail ---\n{self.session.tail()}\n",
        )

        # vt-2's shell moves serial back to vt-1, whose shell then answers.
        self.session.send_command("tty -s 1", timeout_s=20)
        self._command("echo back_on_one", expect=b"back_on_one")
        self._command("tty 0")
        self._set_mode(*boot_mode)


if __name__ == "__main__":
    unittest.main()
