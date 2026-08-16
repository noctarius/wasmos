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

from qemu_test_framework import QemuSession, default_config

# The compositor paints its menu bar in this colour (libui.h mbroot->bg_color),
# across the full width of the desktop's top rows. Nothing the text console draws
# uses it, which is what makes it a reliable "the GUI owns the screen" marker.
MENU_BAR_RGB = (0x1A, 0x22, 0x33)
# How far a sampled pixel may sit from a reference colour and still count as it.
# Exact equality would be correct today; the tolerance is here so a future
# gradient or anti-aliased edge does not turn this into a false alarm.
COLOUR_TOLERANCE = 12
# Sample window: a band across the top of the screen, where the menu bar lives
# and where a text console has at most one row of glyphs on a black field.
SAMPLE_W = 240
SAMPLE_H = 16


def read_ppm(path):
    """Parse a binary PPM (P6) into (width, height, bytes). QEMU's screendump
    writes this format natively, so no image library is involved."""
    with open(path, "rb") as fh:
        data = fh.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM")
    fields = []
    pos = 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace byte after maxval
    width, height, maxval = fields
    if maxval != 255:
        raise ValueError(f"{path}: unsupported maxval {maxval}")
    return width, height, data[pos : pos + width * height * 3]


def sample_band(path):
    """The top-left band of the frame as a list of (r, g, b)."""
    width, height, pixels = read_ppm(path)
    out = []
    for y in range(min(SAMPLE_H, height)):
        row = y * width * 3
        for x in range(min(SAMPLE_W, width)):
            off = row + x * 3
            out.append((pixels[off], pixels[off + 1], pixels[off + 2]))
    return out


def close_to(pixel, reference):
    return all(abs(a - b) <= COLOUR_TOLERANCE for a, b in zip(pixel, reference))


def fraction_matching(band, reference):
    if not band:
        return 0.0
    return sum(1 for px in band if close_to(px, reference)) / len(band)


class VtFramebufferOwnershipTests(unittest.TestCase):
    """The framebuffer has exactly one owner, and it follows the visible slot.

    vt-0 is the compositor's and every other slot is a text console, so the two
    can never be on screen at once. The vt drives that handover with
    VT_IPC_VIS_NOTIFY: the compositor draws only while vt-0 is visible, and the
    framebuffer driver renders vt's cells only while it is not.

    The assertions are made on the pixels, because that is the only place the
    invariant is actually observable — a compositor that keeps drawing over a
    text slot still answers IPC correctly. A band across the top of the frame is
    enough to tell the two apart: the desktop fills it with the menu-bar colour,
    a text console leaves it black but for at most one row of glyphs.
    """

    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.mkdtemp(prefix="wasmos-fbowner-")
        cfg = default_config()
        # screendump needs a real VGA device, which -nographic removes; display
        # "none" keeps it headless.
        cfg.nographic = False
        cfg.display = "none"
        cfg.enable_monitor = True
        cfg.monitor_socket = os.path.join(cls.tmpdir, "mon.sock")
        cls.session = QemuSession(cfg, timeout_s=180, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")
        if not cls.session.settle():
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(
                f"system never went idle after boot\n--- tail ---\n{tail}\n"
            )

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.close()

    def _switch(self, command):
        mark = self.session.mark()
        self.session.send(command)
        if not self.session.expect_from(mark, b"wamos> ", timeout_s=20):
            self.fail(
                f"no prompt after {command!r}.\n--- tail ---\n{self.session.tail()}\n"
            )

    def _shot(self, name):
        path = os.path.join(self.tmpdir, f"{name}.ppm")
        self.session.monitor.screendump(path, fmt="ppm")
        return sample_band(path)

    def _wait_for_band(self, name, predicate, timeout_s=8.0):
        """Re-dump until the band satisfies `predicate`, and return it.

        A repaint is asynchronous: the prompt comes back when the vt has applied
        the switch, while the component taking over the framebuffer paints on its
        own next pass. Polling is the difference between testing the invariant
        and testing how fast the machine happens to be."""
        deadline = time.time() + timeout_s
        band = self._shot(name)
        while not predicate(band) and time.time() < deadline:
            time.sleep(0.5)
            band = self._shot(name)
        return band

    def test_screen_follows_the_visible_slot(self):
        # A text slot: the desktop must not be on screen at all.
        self._switch("tty 1")
        text_band = self._wait_for_band(
            "vt1", lambda b: fraction_matching(b, MENU_BAR_RGB) < 0.10
        )
        menu_fraction = fraction_matching(text_band, MENU_BAR_RGB)
        self.assertLess(
            menu_fraction,
            0.10,
            msg=(
                "the compositor's menu bar is visible while a text slot is on "
                f"screen ({menu_fraction:.0%} of the sampled band).\n"
                f"--- tail ---\n{self.session.tail()}\n"
            ),
        )

        # ...and it stays away. The gfx demos animate continuously, so a
        # compositor that had not relinquished the framebuffer would repaint
        # this band within a second.
        # The screendump travels over the monitor socket, so the serial reader
        # does not need pumping here — and must not be pumped with a doomed
        # expect(), which force-stops the VM on timeout.
        time.sleep(2)
        still_text_band = self._shot("vt1-again")
        menu_fraction = fraction_matching(still_text_band, MENU_BAR_RGB)
        self.assertLess(
            menu_fraction,
            0.10,
            msg=(
                "the compositor repainted over a text slot after the switch "
                f"({menu_fraction:.0%} of the sampled band).\n"
                f"--- tail ---\n{self.session.tail()}\n"
            ),
        )

        # vt-0 is the compositor's: switching there hands the framebuffer over.
        self._switch("tty 0")
        gui_band = self._wait_for_band(
            "vt0", lambda b: fraction_matching(b, MENU_BAR_RGB) > 0.60
        )
        menu_fraction = fraction_matching(gui_band, MENU_BAR_RGB)
        self.assertGreater(
            menu_fraction,
            0.60,
            msg=(
                "the desktop is not on screen after switching to vt-0 "
                f"({menu_fraction:.0%} of the sampled band is menu bar).\n"
                f"--- tail ---\n{self.session.tail()}\n"
            ),
        )
        self.assertNotEqual(
            gui_band,
            text_band,
            msg="vt-0 and vt-1 rendered identical frames; the switch drew nothing",
        )

        # And back: the handover is not one-way.
        self._switch("tty 1")
        back_band = self._wait_for_band(
            "vt1-back", lambda b: fraction_matching(b, MENU_BAR_RGB) < 0.10
        )
        menu_fraction = fraction_matching(back_band, MENU_BAR_RGB)
        self.assertLess(
            menu_fraction,
            0.10,
            msg=(
                "the desktop is still on screen after switching back to a text "
                f"slot ({menu_fraction:.0%} of the sampled band).\n"
                f"--- tail ---\n{self.session.tail()}\n"
            ),
        )


if __name__ == "__main__":
    unittest.main()
