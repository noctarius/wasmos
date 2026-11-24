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
    reason="needs a matched device whose driver finds nothing to drive: QEMU "
    "virt's fw_cfg (QEMU0002) without a ramfb",
)
class DevmgrDeclinedSpawnTest(unittest.TestCase):
    """A driver that exits cleanly before READY has declined its device.

    Regression: 2026-11-24-devmgr-declined-spawn. device-manager treated every
    exit before READY as a failed spawn and retried it, up to its retry limit.
    The ramfb driver, matched on fw_cfg (ACPI QEMU0002), exits with status 0
    when the machine has no ramfb -- a headless boot is not an error -- and was
    spawned nine times per boot, each one a full module load, before
    device-manager gave up with "it never became ready".
    """

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cfg.display_devices = ()
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

    def test_clean_exit_before_ready_is_not_retried(self) -> None:
        buf = bytes(self.session.buf)
        loads = buf.count(b"[native-driver] loading fbramfb")
        self.assertEqual(
            loads,
            1,
            f"the ramfb driver was loaded {loads} times; a clean exit before "
            f"READY must not be retried\n--- tail ---\n{self.session.tail()}",
        )
        self.assertIn(
            b"declined the device",
            buf,
            "device-manager did not report the declined spawn",
        )
        self.assertNotIn(
            b"gave up on system/drivers/fbramfb.wap",
            buf,
            "a declined device was reported as a failed spawn",
        )


if __name__ == "__main__":
    unittest.main()
