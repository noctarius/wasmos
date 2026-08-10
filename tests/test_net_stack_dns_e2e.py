import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config, default_kernel_path


class NetStackDnsE2ETest(unittest.TestCase):
    """Exercise the `ip dns` tool against net-stack's DNS control plane.

    Boots to the CLI, shows the resolver configured from the ifcfg
    `dns-nameservers` line, replaces it via `ip dns set`, confirms `ip dns show`
    reflects the change, then removes one server via `ip dns del`. Proves the
    NET_IPC_DNS_SET/LIST handlers and the ip app end to end. Hermetic: it only
    reads and writes the resolver configuration, never performs a lookup.
    """

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        kernel_src = default_kernel_path()
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

    def test_ip_dns_set_show_del(self) -> None:
        assert self.session is not None
        session = self.session
        self.assertTrue(session.expect(b"wamos> ", timeout_s=120))
        self.assertTrue(
            session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=60),
            "net-stack did not configure the static default",
        )

        # The default ifcfg sets a single resolver (10.0.2.3, the SLIRP DNS).
        show1 = session.mark()
        session.send("spawn /system/utils/ip dns show")
        self.assertTrue(
            session.expect_from(show1, b"[ip] dns 10.0.2.3", timeout_s=30),
            "ip dns show did not report the ifcfg resolver",
        )

        # Replace the resolver list at runtime.
        setm = session.mark()
        session.send("spawn /system/utils/ip dns set 8.8.8.8 1.1.1.1")
        self.assertTrue(
            session.expect_from(setm, b"[ip] dns set ok", timeout_s=30),
            "ip dns set did not succeed",
        )

        show2 = session.mark()
        session.send("spawn /system/utils/ip dns show")
        self.assertTrue(
            session.expect_from(show2, b"[ip] dns 8.8.8.8", timeout_s=30),
            "ip dns show did not reflect the first new server",
        )
        self.assertTrue(
            session.expect_from(show2, b"[ip] dns 1.1.1.1", timeout_s=30),
            "ip dns show did not reflect the second new server",
        )

        # Remove one server; the other must remain.
        delm = session.mark()
        session.send("spawn /system/utils/ip dns del 8.8.8.8")
        self.assertTrue(
            session.expect_from(delm, b"[ip] dns del ok", timeout_s=30),
            "ip dns del did not succeed",
        )
        show3 = session.mark()
        session.send("spawn /system/utils/ip dns show")
        self.assertTrue(
            session.expect_from(show3, b"[ip] dns 1.1.1.1", timeout_s=30),
            "ip dns show did not retain the remaining server",
        )


if __name__ == "__main__":
    unittest.main()
