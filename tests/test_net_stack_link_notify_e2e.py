import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class NetStackLinkNotifyE2ETest(unittest.TestCase):
    """Verify net-stack survives a link down/up cycle without restarting.

    The boot-spawned net-stack binds virtio.net, brings its lwIP netif up, and
    subscribes to driver link events (via LINK_GET). Toggling the emulated NIC
    link through the QEMU monitor flips virtio-net's VIRTIO_NET_S_LINK_UP config
    bit; the driver polls that change and forwards it as NETDRV_IPC_LINK_NOTIFY.

    This is a behavior-level check (not a source-text assertion): net-stack
    reports the netif transition on the console, and the same instance must
    observe both edges. It must NOT re-register `net.stack` or re-run its
    interface bring-up ("eth0 ... ready") banner, which would indicate a
    restart/rebind rather than an in-place link update.
    """

    session: QemuSession | None = None
    READY = b"[net-stack] eth0 10.0.2.15/24 ready"
    REGISTERED = b"[net-stack] registered net.stack"

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        # A monitor is required to drive `set_link` from the test.
        cfg.enable_monitor = True
        kernel_src = os.path.join("build", "kernel.elf")
        kernel_dst = os.path.join(cfg.esp_dir, "kernel.elf")
        if os.path.exists(kernel_src) and os.path.isdir(cfg.esp_dir):
            shutil.copyfile(kernel_src, kernel_dst)
        cls.session = QemuSession(cfg, timeout_s=150)
        cls.session.start()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_link_down_up_preserves_netif(self) -> None:
        assert self.session is not None
        session = self.session

        # Boot to the CLI and confirm the boot-spawned net-stack brought its
        # interface up (link initially up).
        self.assertTrue(session.expect(b"wamos> ", timeout_s=120))
        self.assertTrue(
            session.expect(self.READY, timeout_s=60),
            "net-stack never reported its interface ready",
        )

        # Force the link down; the driver polls the config status change and
        # net-stack must report the netif going down.
        down_mark = session.mark()
        session.set_link(False)
        self.assertTrue(
            session.expect_from(down_mark, b"[net-stack] link down", timeout_s=30),
            "net-stack did not observe the link-down event",
        )

        # Force it back up on the same instance; the netif must recover.
        up_mark = session.mark()
        session.set_link(True)
        self.assertTrue(
            session.expect_from(up_mark, b"[net-stack] link up", timeout_s=30),
            "net-stack did not observe the link-up event",
        )

        # No restart/rebind: the registration and bring-up banners are emitted
        # exactly once for the lifetime of the instance.
        self.assertEqual(
            session.buf.count(self.REGISTERED),
            1,
            "net-stack re-registered net.stack (unexpected restart)",
        )
        self.assertEqual(
            session.buf.count(self.READY),
            1,
            "net-stack re-ran interface bring-up (unexpected rebind)",
        )


if __name__ == "__main__":
    unittest.main()
