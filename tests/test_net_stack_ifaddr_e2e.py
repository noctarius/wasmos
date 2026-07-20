import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class NetStackIfaddrE2ETest(unittest.TestCase):
    """Exercise the `ip` tool against net-stack's IFADDR control plane.

    Boots to the CLI, shows the configured interface, sets a new address with
    `ip addr add`, and confirms `ip addr show` reflects the change. Proves the
    NET_IPC_IFADDR_ADD/LIST handlers and the ip app end to end.
    """

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
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

    def test_ip_addr_add_show(self) -> None:
        assert self.session is not None
        session = self.session
        self.assertTrue(session.expect(b"wamos> ", timeout_s=120))
        # The static default must be up before we inspect/change it.
        self.assertTrue(
            session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=60),
            "net-stack did not configure the static default",
        )

        # Show the current address via the IFADDR_LIST path.
        show1 = session.mark()
        session.send("spawn /system/utils/ip addr show")
        self.assertTrue(
            session.expect_from(show1, b"[ip] eth0: 10.0.2.15/24", timeout_s=30),
            "ip addr show did not report the configured address",
        )

        # Change the address via the IFADDR_ADD path.
        add = session.mark()
        session.send("spawn /system/utils/ip addr add 10.0.2.50/24 dev eth0")
        self.assertTrue(
            session.expect_from(add, b"[ip] addr add ok", timeout_s=30),
            "ip addr add did not succeed",
        )

        # The change must be observable through a fresh list.
        show2 = session.mark()
        session.send("spawn /system/utils/ip addr show")
        self.assertTrue(
            session.expect_from(show2, b"[ip] eth0: 10.0.2.50/24", timeout_s=30),
            "ip addr show did not reflect the new address",
        )


if __name__ == "__main__":
    unittest.main()
