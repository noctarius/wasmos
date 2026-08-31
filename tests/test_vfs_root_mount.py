"""The VFS root is a real filesystem, and mounts are directories in it.

Before this, `/` was a reserved name with no filesystem behind it: `ls /`
printed fs-manager's mount table rather than reading a directory, a mount could
only exist at the top level, and `cd /system` succeeded while `/system` appeared
in no listing. These cases pin the properties that replaced that arrangement:

- `ls /` is an ordinary readdir served by the tmpfs mounted at `/`,
- every mount appears in it as a directory, because fs-manager creates the mount
  point rather than appending mount names to the reply,
- the listing and the mount table agree exactly, in both directions, which an
  invented listing could not guarantee,
- and `ls` agrees with `cd`: a name one accepts is a name the other shows. That
  disagreement is the specific defect the root filesystem exists to fix.
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


class VfsRootMountTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def _run(self, cmd: str, timeout_s: int = 20) -> bytes:
        """Send `cmd` and return everything it printed before the next prompt.

        `mark()` is an index into the session's output buffer, so slicing that
        buffer from it is how a caller reads one command's output rather than the
        whole session's.
        """
        mark = self.session.mark()
        self.session.send(cmd)
        if not self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s):
            self.fail(
                f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )
        return bytes(self.session.buf[mark:])

    def test_root_is_served_by_the_tmpfs(self):
        """`mount` names a filesystem at `/`, which is what makes it a mount.

        Reported from the backend's own FS_TYPE_*, so the line proves the tmpfs
        registered and was routed to, not merely that it started.
        """
        out = self._run("mount")
        self.assertIn(b"/ ->", out, f"no root mount listed\n{out!r}")
        self.assertIn(b"fs-tmpfs", out, f"root not served by the tmpfs\n{out!r}")

    def test_every_mount_appears_in_the_root_listing(self):
        """A mount point is a DIRECTORY in the root filesystem.

        fs-manager creates it when the mount registers, so the entries below are
        read out of the tmpfs rather than appended to the reply by fs-manager --
        which is why `ls /` can now be an ordinary forwarded readdir.
        """
        self._run("cd /")
        listing = self._run("ls")
        for mount in (b"boot", b"init"):
            self.assertIn(
                mount, listing, f"mount {mount!r} missing from `ls /`\n{listing!r}"
            )

    def test_ls_and_cd_agree_at_the_root(self):
        """Every name `ls /` shows is one `cd` accepts, and vice versa.

        This is the defect the root filesystem was built to remove: with `/`
        unbacked, `cd /system` succeeded through a fallback while `ls /` never
        listed `/system`, so the two disagreed about what existed.
        """
        self._run("cd /")
        listing = self._run("ls")
        names = [
            line.strip().rstrip(b"/")
            for line in listing.splitlines()
            if line.strip().endswith(b"/") and b"wamos>" not in line
        ]
        names = [n for n in names if n and n not in (b".", b"..")]
        self.assertTrue(names, f"no directories listed at /\n{listing!r}")
        for name in names:
            out = self._run(b"cd /".decode() + name.decode())
            self.assertIn(
                b"/" + name,
                out,
                f"`ls /` showed {name!r} but `cd` did not accept it\n{out!r}",
            )
            self._run("cd /")

        # And the other direction: a name that is NOT listed is not accepted.
        out = self._run("cd /definitely-not-here")
        self.assertNotIn(
            b"/definitely-not-here wamos>",
            out,
            f"cd accepted a path no listing shows\n{out!r}",
        )

    def test_the_root_listing_is_exactly_the_mounts_and_nothing_invented(self):
        """The directories at `/` are the mount points, and only those.

        This is the property that replaced the invented root listing. fs-manager
        used to append its mount table to the reply, so `ls /` could show names no
        filesystem held; now the entries are directories it created in the tmpfs,
        so the two views must agree in BOTH directions -- every depth-1 mount is
        listed, and nothing is listed that is not one.

        The tmpfs write path itself is covered on the host
        (tests/unit/test_fs_tmpfs_store.zig); the CLI has no mkdir, so there is
        nothing here to write with.
        """
        mounts = self._run("mount")
        expected = set()
        for line in mounts.splitlines():
            line = line.strip()
            if b"->" not in line:
                continue
            path = line.split(b"->")[0].strip()
            if not path.startswith(b"/") or path == b"/":
                continue
            # Only depth-1 mounts appear as entries AT the root.
            segments = [seg for seg in path.split(b"/") if seg]
            if len(segments) == 1:
                expected.add(segments[0])
        self.assertTrue(expected, f"no mounts parsed from `mount`\n{mounts!r}")

        self._run("cd /")
        listing = self._run("ls")
        listed = {
            line.strip().rstrip(b"/")
            for line in listing.splitlines()
            if line.strip().endswith(b"/") and b"wamos>" not in line
        }
        listed.discard(b"")
        listed.discard(b".")
        listed.discard(b"..")

        self.assertEqual(
            expected,
            listed,
            f"`ls /` and `mount` disagree\nmount: {sorted(expected)}\nls: {sorted(listed)}",
        )

    def test_a_mounted_volume_is_still_reachable_through_its_mount_point(self):
        """Routing still prefers the mount over the root that covers it.

        The mount point is a directory in the tmpfs, so a path under it could
        plausibly be served by the tmpfs instead. It must not be: the longest
        mount path owns the path, and /boot is longer than /.
        """
        self._run("cd /boot")
        listing = self._run("ls")
        self.assertIn(
            b"system",
            listing,
            f"/boot did not list the FAT volume's contents\n{listing!r}",
        )
        # If the tmpfs were answering, this would be the EMPTY mount-point
        # directory it holds rather than the volume's contents.
        self.assertNotIn(b"boot/", listing)


class VfsRootWritableTest(unittest.TestCase):
    """The root filesystem accepts writes, exercised through the `mkdir` util.

    Its own session, not a shared one: creating a directory at `/` changes what
    `ls /` reports, and VfsRootMountTest asserts that the root listing is EXACTLY
    the mount points. Sharing a session would make one case decide the other's
    outcome depending on the order unittest happened to pick.
    """

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    _run = VfsRootMountTest._run

    def test_a_directory_created_at_the_root_is_in_its_listing(self):
        """A mount TABLE could not do this, which is what separates "the root is a
        real filesystem" from "the root listing happens to look like one"."""
        self._run("cd /")
        self._run("mkdir rootscratch")
        listing = self._run("ls")
        self.assertIn(
            b"rootscratch",
            listing,
            f"directory created at / is not in its listing\n{listing!r}",
        )
        out = self._run("cd /rootscratch")
        self.assertIn(
            b"/rootscratch wamos>", out, f"cannot stand in the new directory\n{out!r}"
        )

    def test_mkdir_p_creates_missing_parents_and_is_idempotent(self):
        """`mkdir -p` is the case the root filesystem makes reachable at all:
        before it, `/` held nothing and there was no parent to create under."""
        self._run("cd /")
        out = self._run("mkdir -p /deep/a/b")
        self.assertNotIn(b"mkdir:", out, f"mkdir -p reported an error\n{out!r}")
        out = self._run("cd /deep/a/b")
        self.assertIn(b"/deep/a/b wamos>", out, f"deep path not created\n{out!r}")

        # Idempotent: an existing prefix is not an obstacle.
        self._run("cd /")
        out = self._run("mkdir -p /deep/a/b/c")
        self.assertNotIn(
            b"mkdir:", out, f"mkdir -p on an existing prefix failed\n{out!r}"
        )
        out = self._run("cd /deep/a/b/c")
        self.assertIn(b"/deep/a/b/c wamos>", out, f"deeper path not created\n{out!r}")

    def test_mkdir_without_p_refuses_a_missing_parent(self):
        """Without -p the parent must already exist, so the flag is doing work
        rather than being the only code path."""
        self._run("cd /")
        out = self._run("mkdir /nope/child")
        self.assertIn(b"mkdir:", out, f"expected a refusal\n{out!r}")
        out = self._run("cd /nope")
        self.assertNotIn(
            b"/nope wamos>", out, f"the parent was created anyway\n{out!r}"
        )


if __name__ == "__main__":
    unittest.main()
