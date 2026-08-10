import os
import shutil
import unittest

from scripts.qemu_test_framework import QemuSession, default_config, default_kernel_path


class NetStackDnsResolveE2ETest(unittest.TestCase):
    """Resolve a hostname through net-stack's NET_IPC_RESOLVE + the `host` tool.

    Uses the static "localhost" entry in net-stack's DNS local host list, so the
    lookup completes from lwIP's cache with no network round-trip. This exercises
    the full resolver path hermetically: the `host` tool -> shared
    wasmos_net_resolve() client helper -> NET_IPC_RESOLVE handler ->
    dns_gethostbyname() -> reply, and asserts the well-known loopback address.
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

    def test_host_resolves_localhost(self) -> None:
        assert self.session is not None
        session = self.session
        self.assertTrue(session.expect(b"wamos> ", timeout_s=120))
        self.assertTrue(
            session.expect(b"[net-stack] eth0 10.0.2.15/24 ready", timeout_s=60),
            "net-stack did not configure the static default",
        )
        mark = session.mark()
        session.send("spawn /system/utils/host localhost")
        self.assertTrue(
            session.expect_from(mark, b"[host] localhost -> 127.0.0.1", timeout_s=30),
            "host did not resolve localhost through net-stack DNS",
        )


if __name__ == "__main__":
    unittest.main()
