"""The WFS backend, end to end in the guest: a real volume on its own disk is
mounted, listed, walked into, and read.

Everything below the CLI here is exercised for real — the ATA secondary channel,
the block IPC path, the superblock and group descriptors off a physical device,
the directory scan, the extent map and the read path — against an image
mkfs_wfs built from scripts/wfs at build time. The host unit suites cover each of
those layers in isolation; what only a boot can show is that they agree with the
device, the device manager, and fs-manager.
"""

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config

# Content of scripts/wfs/hello.txt, which mkfs_wfs placed in the volume. Small
# enough that WFS stores it INLINE in the object record, so reading it exercises
# the inline path rather than an extent.
HELLO_TEXT = b"hello from the WFS volume"


class WfsMountReadTest(unittest.TestCase):
    """A WFS volume mounts off a real disk and its files can be listed and read."""

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=150, echo=True)
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

        Marks the stream first and expects FROM that mark, so a needle already
        present in the boot log cannot satisfy an assertion about this command.
        """
        mark = self.session.mark()
        self.session.send(cmd)
        for needle in needles:
            if not self.session.expect_from(mark, needle, timeout_s=timeout_s):
                self.fail(
                    f"'{cmd}' did not produce {needle!r}\n"
                    f"--- tail ---\n{self.session.tail()}\n"
                )
        if not self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s):
            self.fail(f"no prompt after '{cmd}'\n--- tail ---\n{self.session.tail()}\n")

    def _cat(self, cmd: str, needles: list[bytes], timeout_s: int = 40) -> None:
        """Run a read command ONCE and require its content.

        One run, not two: a `cat` of an 11 KB file is not worth doing twice, and
        requiring the content already proves the read did not fail. The error
        text is checked in the same pass so a failure reports the reason rather
        than just a missing needle.
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

    def test_the_driver_mounted_the_volume(self):
        """The markers the driver prints on a validated volume.

        'mounted' means the superblock parsed and every group descriptor verified
        off the real device; 'ready' means it registered as an fs-manager backend
        and answered the first info pull. Both come from the boot, so they are
        matched from the start of the log rather than from a mark.
        """
        for marker in (b"[fs-wfs] mounted", b"[fs-wfs] ready"):
            self.assertTrue(
                self.session.expect(marker, timeout_s=30),
                f"{marker!r} absent — the WFS volume did not mount. Check that the "
                f"guest enumerated block unit 2 (ATA secondary channel) and that "
                f"the device-manager rule for it fired.\n"
                f"--- tail ---\n{self.session.tail()}\n",
            )

    def test_the_mount_appears_in_the_root_listing(self):
        """fs-manager routes /wfs, which is what makes the rest reachable."""
        self._cmd("cd /", [])
        self._cmd("ls", [b"wfs/"])

    def test_listing_the_volume_shows_what_mkfs_placed(self):
        """READDIR over the FS_IPC_STREAM frames, against a populated volume.

        Directories carry a trailing '/', matching fs-manager's own root listing.
        """
        self._cmd("cd /", [])
        self._cmd("cd wfs", [])
        self._cmd("ls", [b"docs/", b"etc/", b"hello.txt"])

    def test_walking_into_a_subdirectory(self):
        """A directory reached through the extent map, not the root's own block."""
        self._cmd("cd /", [])
        self._cmd("cd wfs", [])
        self._cmd("cd docs", [])
        self._cmd("ls", [b"big.txt", b"README"])
        # And back out: '..' resolves through the records the directory carries.
        self._cmd("cd ..", [])
        self._cmd("ls", [b"docs/", b"hello.txt"])

    def test_cd_into_a_directory_whose_name_exceeds_fifteen_bytes(self):
        """A name that the old packed CHDIR could not express at all.

        Regression: 2026-08-24-chdir-xfer-buffer

        FS_IPC_CHDIR_REQ used to pack its target into arg0..arg3, capping a name
        at 15 bytes plus a NUL while WFS names run to 255 (WFS_NAME_MAX). The
        request arrived truncated, so the lookup missed and the client was told
        the directory did not exist rather than that its name did not fit. The
        target now travels in a transfer buffer, the same transport OPEN uses.
        """
        self._cmd("cd /", [])
        self._cmd("cd wfs", [])
        self._cmd("cd a-directory-name-past-fifteen-bytes", [])
        self._cmd("ls", [b"marker.txt", b"nested/"])
        self._cmd("cat marker.txt", [b"reached the long-named directory"])
        self._cmd("cd /", [])

    def test_cd_a_deep_path_in_one_command(self):
        """`cd /wfs/<long>/nested/deeper` as a single request.

        Regression: 2026-08-24-chdir-xfer-buffer

        The whole path had to fit in 16 packed bytes, so anything deeper
        truncated; the CLI's workaround sent the remainder in one piece and hit
        the same cap. One buffered request now carries any depth, and fs-manager
        canonicalizes it before routing.
        """
        self._cmd("cd /", [])
        self._cmd("cd /wfs/a-directory-name-past-fifteen-bytes/nested/deeper", [])
        self._cmd("cat deep.txt", [b"reached three levels down"])
        # '..' unwinds the same path it walked in.
        self._cmd("cd ../..", [])
        self._cmd("ls", [b"marker.txt", b"nested/"])
        self._cmd("cd /", [])

    def test_writing_through_the_driver_reads_back(self):
        """The write path, reached the way an application reaches it.

        The host suites cover wfs_write.c and wfs_truncate.c as tasks. This covers
        what they cannot: that the driver's FS IPC dispatch reaches them, that a
        write survives fs-manager's routing and the client transfer buffer, and
        that a SEPARATE open reads back what landed.

        wfs_write_smoke exercises an inline file, a write straddling a block
        boundary, a read-only fd refusing a write, and O_CREAT being refused --
        printing one line per failure, so a red run says which step broke.
        """
        self._cmd("cd /", [])
        self._cmd("wfs_write_smoke", [b"wfs-write-smoke: ok"], timeout_s=60)

    def test_reading_a_file_by_relative_name(self):
        """`cat hello.txt` from inside the mount — the shape a user types.

        Regression: 2026-08-24-cwd-full-vfs-path

        `cat` is a separate process, so this passes only if it resolves the name
        against the directory its spawner stood in. It did not: fs-manager held a
        working directory as (mount, depth) and forwarded a relative name to a
        backend unresolved, choosing that backend by falling back to the first
        boot-kind one when the client had none. On /boot and /init that fallback
        is the right backend and relative `cat` worked by accident; here the FAT
        driver answered NOT_FOUND and this driver was never asked.

        tests/test_fs_open_smoke.py holds the FAT counterpart, so a regression
        that breaks only non-boot mounts is still caught by one of the two.
        """
        self._cmd("cd /", [])
        self._cmd("cd wfs", [])
        self._cat("cat hello.txt", [HELLO_TEXT])

    def test_reading_a_file_by_absolute_path(self):
        """The same file named absolutely, which takes a different code path
        through fs-manager's mount stripping."""
        self._cmd("cd /", [])
        self._cat("cat /wfs/hello.txt", [HELLO_TEXT])

    def test_reading_a_file_larger_than_one_block(self):
        """scripts/wfs/docs/big.txt is ~11 KB, so it spans several 4096-byte
        blocks and is mapped by an extent rather than stored inline. Reading it
        crosses block boundaries on a real device."""
        self._cmd("cd /", [])
        # First and last lines the generator wrote, so a truncated read fails.
        self._cat("cat /wfs/docs/big.txt", [b"line 0000:", b"line 0199:"], timeout_s=60)


if __name__ == "__main__":
    unittest.main()
