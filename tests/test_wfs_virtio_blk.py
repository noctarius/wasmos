"""One WFS volume, two transports: the same image mounts over virtio-blk as well
as ATA, and reads identically through both.

This is what the disk adapter is for. WFS talks to a `block` class instance, not
to a controller, so the backend underneath it should not be observable in the
filesystem's behaviour. The volume here is the very same build/wfs.img the ATA
test uses, attached a second time as a virtio-blk device, so any difference the
tests see is a difference in the transport rather than in the bytes.

It also puts a THIRD block_fs rule in the boot rule set. Rules after the first
are the ones that lost their startup arguments to the device manager's
arm-then-clear ordering (Regression: 2026-08-26-second-block-fs-rule-loses-args),
so a driver here that cannot name its disk is that defect returning.
"""

import os
import sys
import unittest
from dataclasses import replace

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config

# Content of scripts/wfs/hello.txt as mkfs_wfs placed it in the volume. Stored
# INLINE in the object record at this size, so reading it over virtio exercises
# the inline path; docs/big.txt below crosses a block boundary and exercises the
# extent path, and the two together cover both read shapes over the new
# transport.
HELLO_TEXT = b"hello from the WFS volume"

# PCI slot 6 function 0. A virtio-blk unit is (slot << 3) | function, so this
# disk is unit 48 -- the unit the /vwfs device-manager rule names. The slot is
# pinned rather than left to QEMU because the rule matches on the unit and
# nothing else: there is no matcher that reads a volume signature, so the slot IS
# the identity. test_virtio_blk pins its signature disk to slot 5 for the same
# reason, and the two must not collide.
WFS_VIRTIO_ADDR = "0x6.0"
WFS_VIRTIO_UNIT = 48


def _config_with_wfs_over_virtio(wfs_image: str):
    """The standard device set plus build/wfs.img as a virtio-blk disk.

    The image stays attached over ATA as well, so both mounts come up in one
    boot and can be compared against each other rather than across runs.

    snapshot=on for the same reason the ATA WFS drive carries it: a mount marks
    the volume dirty and the guest writes to it, so without a throwaway overlay
    the next boot mounts a volume the previous one left dirty -- which WFS
    correctly mounts read-only, there being no journal replay -- and the read
    fixtures stop holding the bytes asserted here. Two overlays over one backing
    file also keep the two mounts from writing through to each other, which is
    what lets both exist at once.
    """
    return replace(
        default_config(),
        extra_args=(
            "-drive",
            f"file={wfs_image},format=raw,if=none,snapshot=on,id=vwfs0",
            "-device",
            f"virtio-blk-pci,drive=vwfs0,id=vwfs,addr={WFS_VIRTIO_ADDR}",
        ),
    )


class WfsOverVirtioBlkTest(unittest.TestCase):
    """A WFS volume mounts and reads over virtio-blk, matching the ATA mount."""

    session = None

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        if not cfg.wfs_image or not os.path.exists(cfg.wfs_image):
            raise unittest.SkipTest(
                f"no WFS image at {cfg.wfs_image!r}; build the wfs_image target"
            )
        cls.session = QemuSession(
            _config_with_wfs_over_virtio(cfg.wfs_image), timeout_s=180, echo=True
        )
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.force_stop()
            cls.session.close()

    def _cmd(self, cmd: str, needles: list[bytes], timeout_s: int = 25) -> None:
        """Run `cmd` and require each needle, then the prompt back.

        Marks the stream first and expects FROM that mark, so a needle already in
        the boot log cannot satisfy an assertion about this command.
        """
        mark = self.session.mark()
        self.session.send(cmd)
        for needle in needles:
            self.assertTrue(
                self.session.expect_from(mark, needle, timeout_s=timeout_s),
                f"{cmd!r} did not produce {needle!r}\n"
                f"--- tail ---\n{self.session.tail()}\n",
            )
        self.assertTrue(
            self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s),
            f"{cmd!r} never returned to the prompt\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )

    def _cat(self, cmd: str, needles: list[bytes], timeout_s: int = 40) -> None:
        """Run a read command ONCE and require its content.

        One run, not two: requiring the content already proves the read did not
        fail, and the error text is checked in the same pass so a failure reports
        the reason rather than just a missing needle.
        """
        mark = self.session.mark()
        self.session.send(cmd)
        for needle in needles:
            if not self.session.expect_from(mark, needle, timeout_s=timeout_s):
                failed = self.session.expect_from(mark, b"fs failed", timeout_s=1)
                self.fail(
                    f"'{cmd}' did not produce {needle!r}"
                    + (" — it reported 'fs failed'" if failed else "")
                    + f"\n--- tail ---\n{self.session.tail()}\n"
                )

    def test_the_driver_claims_the_disk_as_its_own_instance(self):
        """The virtio disk registers as (virtio-blk, 48), distinct from ATA's."""
        self._cmd(
            "blkinfo",
            [
                b"instance=%d driver=virtio-blk unit=%d"
                % (
                    (2 << 8) | WFS_VIRTIO_UNIT,
                    WFS_VIRTIO_UNIT,
                )
            ],
            timeout_s=60,
        )

    def test_the_volume_mounts_over_virtio(self):
        """fs_wfs mounted the virtio disk, not only the ATA one.

        Both mounts report the same marker, so this asserts the count: one for
        /wfs over ATA and one for /vwfs over virtio. A single occurrence means the
        virtio rule did not fire or its driver refused the volume.
        """
        self.assertGreaterEqual(
            self.session.buf.count(b"[fs-wfs] mounted"),
            2,
            "only one WFS mount came up, so the volume mounted over one transport "
            "and not the other\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )

    def test_the_driver_was_told_which_disk_it_is(self):
        """Every block_fs rule carried its driver=/unit= args, this being the third.

        Regression: 2026-08-26-second-block-fs-rule-loses-args. The device manager
        armed each rule after the first from inside the previous rule's completion
        handler and then cleared the active-spawn tracking on top of it; the
        downgraded spawn took an opcode that carries no arguments, so the driver
        could not name its disk and parked. A third rule is more pressure on that
        ordering than the two that found it.
        """
        self.assertNotIn(
            b"[fs-wfs] startup args missing",
            self.session.buf,
            "a WFS driver was spawned without driver=/unit= and parked instead of "
            "mounting\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )
        self.assertNotIn(
            b"rule spawn armed without a kind",
            self.session.buf,
            "a rule spawn reached the dispatcher with its kind cleared, which "
            "silently drops the startup args\n"
            f"--- tail ---\n{self.session.tail()}\n",
        )

    def test_the_virtio_mount_appears_in_the_root_listing(self):
        """fs-manager routes /vwfs, which is what makes the rest reachable."""
        self._cmd("cd /", [])
        self._cmd("ls", [b"vwfs"])

    def test_reading_an_inline_file_over_virtio(self):
        """The inline read path works with a virtio disk underneath it."""
        self._cmd("cd /vwfs", [])
        self._cmd("cat hello.txt", [HELLO_TEXT])

    def test_the_two_transports_read_the_same_bytes(self):
        """One image, two mounts, identical content.

        The point of the disk adapter: the filesystem addresses a `block` class
        instance, so which controller carries the sectors must not be observable
        above it. Both the listing and a file body are checked over each mount,
        because a transport bug that corrupted data while leaving the directory
        intact would pass a listing-only check.
        """
        self._cmd("cd /wfs", [])
        self._cmd("ls", [b"docs/", b"etc/", b"hello.txt"])
        self._cat("cat /wfs/hello.txt", [HELLO_TEXT])

        self._cmd("cd /vwfs", [])
        self._cmd("ls", [b"docs/", b"etc/", b"hello.txt"])
        self._cat("cat /vwfs/hello.txt", [HELLO_TEXT])

    def test_reading_across_a_block_boundary_over_virtio(self):
        """docs/big.txt is ~11 KB, so it spans several 4096-byte blocks and is
        mapped by an extent rather than stored inline. Reading it over virtio
        crosses block boundaries on a device the extent path has not seen before.

        The first and last lines the generator wrote are both required, so a
        truncated read fails rather than passing on the head of the file.
        """
        self._cmd("cd /", [])
        self._cat(
            "cat /vwfs/docs/big.txt", [b"line 0000:", b"line 0199:"], timeout_s=60
        )


if __name__ == "__main__":
    unittest.main()
