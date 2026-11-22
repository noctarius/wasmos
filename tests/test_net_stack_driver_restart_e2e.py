import os
import shutil
import socket
import unittest

from scripts.qemu_test_framework import QemuSession, default_config, default_kernel_path

# Host port forwarded by QEMU SLIRP into the guest listener (10.0.2.15:5571).
HOST_PORT = 5572
GUEST_PORT = 5571
PAYLOAD = b"wasmos-after-restart"
READY = b"[net-stack] eth0 10.0.2.15/24 ready"
DRIVER_OK = "[virtio-net] driver ok"


class NetStackDriverRestartE2ETest(unittest.TestCase):
    """net-stack keeps eth0 -- its address and its sockets -- across a restart of
    virtio-net, and rebinds it to the successor.

    The guest listener is opened before the driver is killed and the host
    connects only after the rebind, so the accept and the echo cross the new
    driver instance on a socket, an address and a netif that predate it.
    """

    session: QemuSession | None = None
    nic_model = "virtio-net-pci"

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cfg.nic_model = cls.nic_model
        cfg.netdev = (
            f"user,id=net0,hostfwd=tcp:127.0.0.1:{HOST_PORT}-10.0.2.15:{GUEST_PORT}"
        )
        kernel_src = default_kernel_path()
        kernel_dst = os.path.join(cfg.esp_dir, "kernel.elf")
        if os.path.exists(kernel_src) and os.path.isdir(cfg.esp_dir):
            shutil.copyfile(kernel_src, kernel_dst)
        cls.session = QemuSession(cfg, timeout_s=180)
        cls.session.start()
        cls.session.require_prompt(timeout_s=120)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def _unit(self) -> str:
        assert self.session is not None
        start = self.session.send_command("svc list", timeout_s=20)
        self.assertTrue(self.session.expect_from(start, b"wamos> ", timeout_s=30))
        listing = self.session.buf[start:].decode("utf-8", errors="replace")
        units = [
            line.split()[0]
            for line in listing.splitlines()
            if line.startswith("virtio_net@")
        ]
        self.assertEqual(
            len(units), 1, f"virtio-net is not one supervised unit:\n{listing}"
        )
        return units[0]

    def _pid_of(self, name: str) -> str:
        assert self.session is not None
        start = self.session.send_command("ps", timeout_s=20)
        self.assertTrue(self.session.expect_from(start, b"wamos> ", timeout_s=30))
        for line in (
            self.session.buf[start:].decode("utf-8", errors="replace").splitlines()
        ):
            fields = line.split()
            if len(fields) >= 2 and name in fields[1:]:
                return fields[0]
        return ""

    def test_a_listener_survives_a_restart_of_the_nic_driver(self) -> None:
        assert self.session is not None
        session = self.session
        self.assertTrue(session.expect(READY, timeout_s=90))
        unit = self._unit()
        stack_pid = self._pid_of("net-stack")
        self.assertTrue(stack_pid, "ps lists net-stack")

        session.send("spawn /boot/apps/net_tcp_server")
        self.assertTrue(
            session.expect(b"[net-tcp-srv] listening", timeout_s=60),
            "server did not reach the listening state",
        )

        started = session.syslog_tail(1 << 20).count(DRIVER_OK)
        start = session.send_command(f"svc kill {unit}", timeout_s=20)
        self.assertTrue(
            session.expect_from(start, f"svc: killing {unit}".encode(), timeout_s=30),
            f"the kill was not sent.\n--- tail ---\n{session.tail()}\n",
        )
        self.assertTrue(
            session.syslog_wait_count(DRIVER_OK, started + 1, 60),
            "virtio-net did not come back\n"
            f"--- syslog ---\n{session.syslog_tail(4096)}\n",
        )

        # A SYN that reaches the guest before net-stack has rebound eth0 is
        # lost like any frame while the driver is down; the host retransmits
        # it within the connect timeout.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
            client.settimeout(60)
            client.connect(("127.0.0.1", HOST_PORT))
            self.assertTrue(
                session.expect(b"[net-tcp-srv] accepted", timeout_s=60),
                "the listener opened before the restart did not accept\n"
                f"--- syslog ---\n{session.syslog_tail(4096)}\n",
            )
            client.sendall(PAYLOAD)
            echoed = b""
            while len(echoed) < len(PAYLOAD):
                chunk = client.recv(len(PAYLOAD) - len(echoed))
                if not chunk:
                    break
                echoed += chunk
            self.assertEqual(
                echoed, PAYLOAD, "the echo did not cross the restarted driver"
            )

        self.assertEqual(
            bytes(session.buf).count(READY),
            1,
            "eth0 was configured again: the interface did not carry over",
        )
        log = session.syslog_tail(1 << 20)
        self.assertEqual(self._pid_of("net-stack"), stack_pid, "net-stack restarted")
        self.assertNotIn(f"[supervisor] {unit} failed", log)


class NetStackDriverRestartIntxE2ETest(NetStackDriverRestartE2ETest):
    """The same restart with virtio-net on its INTx fallback: the successor
    finds its function quiesced and asks pci-bus to arm it for its pin
    (PCI_IPC_INTX_ENABLE_REQ) before it transmits.

    Not a regression test for that request: under QEMU the device still moves
    frames with bus mastering off, and the INTx path drains the RX ring on a
    timer, so the restart succeeded without it. It covers the successor holding
    transmits until pci-bus answers and then serving."""

    nic_model = "virtio-net-pci,vectors=0"

    def test_a_listener_survives_a_restart_of_the_nic_driver(self) -> None:
        super().test_a_listener_survives_a_restart_of_the_nic_driver()
        assert self.session is not None
        self.assertEqual(
            bytes(self.session.buf).count(b"[virtio-net] function armed for intx"),
            2,
            "a driver instance did not have its function armed for INTx",
        )


if __name__ == "__main__":
    unittest.main()
