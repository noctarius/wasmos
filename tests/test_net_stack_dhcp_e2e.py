import os
import shutil
import tempfile
import unittest

from scripts.qemu_test_framework import QemuSession, default_config


class NetStackDhcpE2ETest(unittest.TestCase):
    """Verify net-stack honors `iface eth0 inet dhcp` in /boot/system/net/interfaces.

    Builds a throwaway copy of the ESP with the interfaces file overridden to
    DHCP, boots it, and asserts net-stack requests a lease and comes up with the
    address handed out by QEMU's built-in SLIRP DHCP server (10.0.2.15). This
    exercises the DHCP code path (lwIP dhcp_start) rather than the static apply,
    while sharing the same `... ready` completion banner.
    """

    session: QemuSession | None = None
    _esp_tmp: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        # Self-contained ESP copy so we can rewrite the interfaces file without
        # disturbing the shared build/esp tree used by other tests.
        cls._esp_tmp = tempfile.mkdtemp(prefix="wasmos-dhcp-esp-")
        esp_dir = os.path.join(cls._esp_tmp, "esp")
        shutil.copytree(cfg.esp_dir, esp_dir)
        kernel_src = os.path.join("build", "kernel.elf")
        if os.path.exists(kernel_src):
            shutil.copyfile(kernel_src, os.path.join(esp_dir, "kernel.elf"))
        net_dir = os.path.join(esp_dir, "system", "net")
        os.makedirs(net_dir, exist_ok=True)
        with open(os.path.join(net_dir, "interfaces"), "w", encoding="utf-8") as handle:
            handle.write("iface eth0 inet dhcp\n")
        cfg.esp_dir = esp_dir
        cfg.isolate_esp = False
        cls.session = QemuSession(cfg, timeout_s=150)
        cls.session.start()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None
        if cls._esp_tmp:
            shutil.rmtree(cls._esp_tmp, ignore_errors=True)
            cls._esp_tmp = None

    def test_dhcp_lease_brings_interface_up(self) -> None:
        assert self.session is not None
        session = self.session
        self.assertTrue(session.expect(b"wamos> ", timeout_s=120))
        # DHCP path was taken (not the static apply).
        self.assertTrue(
            session.expect(b"[net-stack] dhcp: requesting lease", timeout_s=60),
            "net-stack did not start DHCP from the dhcp interfaces config",
        )
        # SLIRP's DHCP server leases 10.0.2.15; the interface must bind and the
        # shared completion banner must fire (not the strict no-lease path).
        self.assertTrue(
            session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=60),
            "net-stack did not obtain a DHCP lease / bind the interface",
        )
        self.assertNotIn(b"[net-stack] dhcp: no lease", session.buf)


if __name__ == "__main__":
    unittest.main()
