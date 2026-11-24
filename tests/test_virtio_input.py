import os
import sys
import time
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
    reason="QEMU virt's virtio-input keyboard and mouse; x86_64 keeps PS/2",
)
class VirtioInputTest(unittest.TestCase):
    """Keyboard and mouse on a machine without a PS/2 controller.

    QEMU virt carries a virtio-keyboard-pci and a virtio-mouse-pci
    (toolchains.ARCHES["aarch64"].qemu_input). device-manager spawns one
    virtio_input instance per function, and each takes the role its device's
    event bitmaps describe: "kbd" for the keyboard, "mouse" for the mouse.

    The keyboard is exercised end to end. Keys typed through QMP reach the
    device, the driver turns evdev codes into PS/2 Set 1 scancodes, and the vt
    decodes them. The vt keeps the keyboard on slot 0 (the compositor) until
    F2 moves the visible slot to tty1, whose CLI is also the one bound to
    serial, so what is typed on the keyboard is echoed on the wire.
    """

    session: QemuSession

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cfg.enable_monitor = True
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

    def test_both_instances_take_their_roles(self) -> None:
        for role in (b"keyboard", b"mouse"):
            if not self.session.expect(
                b"[virtio-input] " + role + b" on ", timeout_s=1
            ):
                self.fail(
                    f"no virtio-input instance came up as the {role.decode()}\n"
                    f"--- tail ---\n{self.session.tail()}"
                )

    def test_typed_keys_reach_the_tty1_cli(self) -> None:
        mon = self.session.monitor
        if mon is None:
            self.skipTest("QMP monitor not connected")
        mon.key_press("f2")
        time.sleep(0.5)
        mark = self.session.mark()
        # Mixed case: Shift travels as its own key, so the uppercase letters
        # are only right if the modifier's make and break codes are. Only keys
        # that sit in the same place on every layout the vt loads (QMP types US
        # positions; the default keymap is de-nodeadkeys, which swaps Y/Z and
        # moves the punctuation).
        mon.type_text("echo VinOkMark\n", delay_s=0.05)
        if not self.session.expect_from(mark, b"VinOkMark", timeout_s=20):
            self.fail(
                "text typed on the virtio keyboard did not reach tty1's CLI\n"
                f"--- tail ---\n{self.session.tail()}"
            )


if __name__ == "__main__":
    unittest.main()
