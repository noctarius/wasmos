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
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, default_config


class VfsSession:
    """Session plumbing shared by the classes below.

    Not a TestCase, so unittest does not try to run it: each concrete class boots
    its OWN session, because these cases change what `/` contains and sharing one
    would make the order unittest happens to pick decide their outcomes.
    """

    # Set by _await_mount_table when the shell refuses to spawn `mount`, so
    # setUpClass can report the refusal rather than only its own timeout.
    spawn_failure = None

    @classmethod
    def setUpClass(cls):
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=120, echo=True)
        cls.session.start()
        if not cls.session.expect(b"wamos> "):
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(f"CLI prompt not detected\n--- tail ---\n{tail}\n")
        if not cls._await_mount_table():
            tail = cls.session.tail()
            refusal = getattr(cls, "spawn_failure", None)
            cls.session.close()
            if refusal:
                raise RuntimeError(
                    f"the `mount` utility could not be spawned, so the mount "
                    f"table was never readable: {refusal!r}\n--- tail ---\n{tail}\n"
                )
            raise RuntimeError(f"mount table never settled\n--- tail ---\n{tail}\n")

    @classmethod
    def tearDownClass(cls):
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    @classmethod
    def _await_mount_table(cls, tries: int = 40) -> bool:
        """Wait until the mount table holds the root and has stopped changing.

        Backends are discovered through `fs.backend` class events, so they
        register ASYNCHRONOUSLY and the CLI prompt can appear while volumes are
        still arriving. A case reading the table before it settles sees a partial
        one, and a case comparing `ls /` against `mount` can see a mount appear
        between the two commands. Both were flaky under battery load, where the
        window is widest, while passing standalone every time.

        Settled means two consecutive reads agree AND the root is present, so this
        cannot return on an empty table that has simply not started filling.
        """
        previous = None
        for _ in range(tries):
            mark = cls.session.mark()
            cls.session.send("mount")
            # Past the echo before looking for the prompt, for the reason _run
            # documents: the prompt precedes the echoed command on one line, so
            # searching from `mark` can match it and yield an empty reading.
            if not cls.session.expect_from(mark, b"mount", timeout_s=10):
                continue
            echo_end = bytes(cls.session.buf).index(b"mount", mark) + len("mount")
            if not cls.session.expect_from(echo_end, b"wamos> ", timeout_s=10):
                continue
            out = bytes(cls.session.buf[echo_end:])
            # `mount` is a spawned utility, not a shell built-in, so a session
            # that cannot spawn reports nothing here and every retry looks
            # identical to a table that is merely slow. Surfacing the shell's own
            # refusal turns "never settled" -- which says only that this loop gave
            # up -- into the reason it did.
            if b"exec failed" in out:
                cls.spawn_failure = out
                return False
            if b"/ ->" in out and out == previous:
                return True
            previous = out
            time.sleep(0.5)
        return False

    def _run(self, cmd: str, timeout_s: int = 20) -> bytes:
        """Send `cmd` and return everything it printed before the next prompt.

        `mark()` is an index into the session's output buffer, so slicing that
        buffer from it is how a caller reads one command's output rather than the
        whole session's.

        The echo has to be stepped over first. The shell prints its prompt and
        the echoed command on the SAME line ("/ wamos> mount"), so scanning for a
        prompt from `mark` can match the one that PRECEDES this command and
        return while only the echo has arrived -- leaving a caller to parse an
        empty result as a real answer. Whether that happens is pure timing, which
        is why it surfaced under battery load and never standalone.
        """
        mark = self.session.mark()
        self.session.send(cmd)
        needle = cmd.encode()
        if not self.session.expect_from(mark, needle, timeout_s=timeout_s):
            self.fail(
                f"'{cmd}' was never echoed.\n--- tail ---\n{self.session.tail()}\n"
            )
        # Resume the prompt search after the echo, not after the mark.
        echo_end = bytes(self.session.buf).index(needle, mark) + len(needle)
        if not self.session.expect_from(echo_end, b"wamos> ", timeout_s=timeout_s):
            self.fail(
                f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )
        return bytes(self.session.buf[echo_end:])


class VfsRootMountTest(VfsSession, unittest.TestCase):
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
        """The directories at `/` are the mount points and their ancestors, only.

        This is the property that replaced the invented root listing. fs-manager
        used to append its mount table to the reply, so `ls /` could show names no
        filesystem held; now the entries are directories it created in the tmpfs,
        so the two views must agree in BOTH directions -- every mount contributes
        its first segment, and nothing is listed that no mount accounts for.

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
            # The FIRST segment of every mount path is what appears at the root:
            # a depth-1 mount as its own point, and a deeper one as the ancestor
            # directory fs-manager created on the way to it. `/home/user` puts
            # `home` here without `/home` being a mount.
            segments = [seg for seg in path.split(b"/") if seg]
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


class VfsDeepAndNestedMountTest(VfsSession, unittest.TestCase):
    """Mounts at depth, mounts inside mounts, and shadowing.

    Three tmpfs instances are placed by rule: `/`, `/home/user` and `/wfs/nested`.
    They need no disk, which is what makes these cases affordable -- a tmpfs is
    told where it belongs by `ENV{MOUNT}` on a boot rule, so the fixtures every
    other test boots against are untouched.

    The two non-root mounts exercise different paths through
    `fsmgr_ensure_mount_points`: `/home/user` has its ancestors created as ordinary
    directories in the ROOT filesystem, while `/wfs/nested` has its mount point
    created inside the WFS VOLUME. The second is also what makes shadowing
    testable at all -- the volume already holds a file there
    (`scripts/wfs/nested/covered.txt`), so the mount covers real content rather
    than an empty directory.
    """

    def test_a_mount_at_depth_is_reported_and_reachable(self):
        out = self._run("mount")
        self.assertIn(b"/home/user ->", out, f"deep mount not registered\n{out!r}")
        out = self._run("cd /home/user")
        self.assertIn(b"/home/user wamos>", out, f"cannot stand in it\n{out!r}")

    def test_the_ancestors_of_a_deep_mount_exist_in_the_root_filesystem(self):
        """`/home` is nobody's mount, so it can only be a directory fs-manager
        created in the tmpfs at `/` while walking the mount path."""
        self._run("cd /")
        listing = self._run("ls")
        self.assertIn(b"home/", listing, f"/home was not created at /\n{listing!r}")
        out = self._run("cd /home")
        self.assertIn(b"/home wamos>", out, f"/home is not a directory\n{out!r}")
        listing = self._run("ls")
        self.assertIn(
            b"user/", listing, f"/home/user is not an entry of /home\n{listing!r}"
        )

    def test_a_deep_mount_is_writable_and_separate_from_the_root(self):
        """A distinct instance, not the root answering for a deeper path: a file
        made in one is absent from the other."""
        self._run("cd /home/user")
        self._run("mkdir deepscratch")
        listing = self._run("ls")
        self.assertIn(b"deepscratch", listing, f"not writable\n{listing!r}")
        self._run("cd /")
        listing = self._run("ls")
        self.assertNotIn(
            b"deepscratch", listing, f"the root and /home/user share state\n{listing!r}"
        )

    def test_a_mount_inside_a_mount_is_reported_and_reachable(self):
        out = self._run("mount")
        self.assertIn(b"/wfs/nested ->", out, f"nested mount not registered\n{out!r}")
        self.assertIn(b"/wfs ->", out, f"the covering mount is gone\n{out!r}")
        out = self._run("cd /wfs/nested")
        self.assertIn(b"/wfs/nested wamos>", out, f"cannot stand in it\n{out!r}")

    def test_the_nested_mount_point_exists_inside_the_covering_volume(self):
        """`nested` is an entry of the WFS volume, not of the root filesystem.

        fs-manager routed the mount path's parent to the WFS backend and created
        the directory THERE, which is the branch a top-level mount never takes.
        """
        self._run("cd /wfs")
        listing = self._run("ls")
        self.assertIn(b"nested/", listing, f"mount point missing in /wfs\n{listing!r}")
        self._run("cd /")
        listing = self._run("ls")
        self.assertNotIn(
            b"nested/", listing, f"the point was created at / instead\n{listing!r}"
        )

    def test_the_nested_mount_shadows_what_the_volume_holds_there(self):
        """The covered file is unreachable while the mount stands.

        `/wfs/nested/covered.txt` exists in the WFS image. With a tmpfs mounted
        over `/wfs/nested`, a path under it reaches the tmpfs, so the file is not
        listed and not readable -- which is the Linux rule, and what keeps mounting
        a property of the namespace rather than of the covered filesystem's state.
        """
        self._run("cd /wfs/nested")
        listing = self._run("ls")
        self.assertNotIn(
            b"covered.txt",
            listing,
            f"the covered file is still listed, so nothing is shadowed\n{listing!r}",
        )
        out = self._run("cat /wfs/nested/covered.txt")
        self.assertNotIn(
            b"exists only to be covered",
            out,
            f"the covered file was readable THROUGH the mount\n{out!r}",
        )

    def test_the_covering_volume_is_otherwise_untouched(self):
        """Shadowing hides one directory, not the volume. If the whole of /wfs
        were being served by the tmpfs, this would be empty."""
        self._run("cd /wfs")
        listing = self._run("ls")
        self.assertIn(b"hello.txt", listing, f"/wfs lost its own content\n{listing!r}")


class VfsChdirAcrossBackendsTest(VfsSession, unittest.TestCase):
    """`cd` asks the backend whether the target is a DIRECTORY, on every backend.

    fs-manager validates a chdir with FS_IPC_STAT_REQ and reads the type out of
    the mode the backend reports. That makes the mode's type bits load-bearing
    for something a user does constantly, so every backend has to report them --
    and two did not: WFS returned the on-disk permission bits with no type at all,
    and fs-init implemented no STAT whatever. Both were invisible while a chdir
    was validated by sending the backend a CHDIR and letting it decide.

    The two halves are a pair on purpose. The first says a directory is accepted,
    the second says a FILE is not; a backend that answered "directory" for
    everything would pass the first alone.

    Regression: 2026-09-01-stat-must-report-the-file-type
    """

    def test_a_directory_is_enterable_on_every_backend(self):
        for path, marker in (
            ("/", b"/ wamos>"),
            ("/init", b"/init wamos>"),
            ("/boot", b"/boot wamos>"),
            ("/wfs", b"/wfs wamos>"),
            ("/home/user", b"/home/user wamos>"),
        ):
            out = self._run(f"cd {path}")
            self.assertIn(marker, out, f"cd {path} was refused\n{out!r}")
        self._run("cd /")

    def test_a_file_is_not_enterable_on_every_backend(self):
        """The mutation guard: a backend reporting S_IFDIR unconditionally, or
        fs-manager skipping the type check, passes the case above and fails here.

        Each path is a regular file that exists, so a refusal can only come from
        the type -- NOT_FOUND would mean the case stopped testing what it says.
        """
        for path in ("/wfs/hello.txt", "/boot/system/services/cli.wap"):
            out = self._run(f"cd {path}")
            # The shell prints the error's DESCRIPTION, so the fragments below
            # are what NOT_DIR and NOT_FOUND read as at the prompt.
            self.assertIn(
                b"must be a directory",
                out,
                f"cd {path} did not refuse on the file type\n{out!r}",
            )
            self.assertNotIn(
                b"does not exist",
                out,
                f"the file is missing, so this case proves nothing\n{out!r}",
            )
        self._run("cd /")


class VfsMountRequestTest(VfsSession, unittest.TestCase):
    """Establishing a mount at runtime, not by boot rule.

    Its own session: these cases ADD mounts, and a mount another class asserts is
    absent must not have been created by this one.

    Regression: 2026-09-01-mount-is-a-request
    Placement was only ever a boot-rule property -- a filesystem went where
    whoever spawned its driver said, and nothing could add one to a running
    system. `mount` reported a table it had no way to change.

    Every mount these cases create is removed again in tearDownClass. A backend
    does not exit when its mount is dropped (docs/TASKS.md), so each one left
    behind holds a process slot for the rest of the session -- and the cases
    below deliberately create several. Cleaning up keeps this class from taxing
    whatever runs after it in the same battery.
    """

    #: Mounts these cases establish, removed in tearDownClass. Listed rather than
    #: discovered so a case that fails midway still has its mount cleaned up.
    CREATED_MOUNTS = ("/scratch", "/scratch2", "/roundtrip")

    @classmethod
    def tearDownClass(cls):
        # Unmount before the session goes down: unlike the other classes here,
        # this one adds to the namespace, and `umount` is the only thing that
        # releases the driver behind a mount.
        if cls.session:
            for path in cls.CREATED_MOUNTS:
                mark = cls.session.mark()
                cls.session.send(f"umount {path}")
                cls.session.expect_from(mark, b"wamos> ", timeout_s=20)
        super().tearDownClass()

    def test_a_tmpfs_can_be_mounted_at_a_new_path(self):
        """fs-manager spawns the driver and the mount is in the table when the
        request is answered -- not whenever the class event happens to arrive."""
        self._run("cd /")
        out = self._run("mount -t tmpfs /scratch", timeout_s=40)
        self.assertIn(b"mounted", out, f"mount refused\n{out!r}")

        out = self._run("mount")
        self.assertIn(b"/scratch ->", out, f"not in the mount table\n{out!r}")
        self.assertIn(b"fs-tmpfs", out, f"not served by a tmpfs\n{out!r}")

    def test_the_new_mount_point_is_a_directory_and_the_mount_is_writable(self):
        """The mount POINT is created by fs-manager in the filesystem covering it,
        the same walk a boot-rule mount takes, and the mount holds its own state:
        a file made in it is absent from the root."""
        self._run("cd /")
        out = self._run("mount -t tmpfs /scratch2", timeout_s=40)
        self.assertIn(b"mounted", out, f"mount refused\n{out!r}")
        listing = self._run("ls")
        self.assertIn(b"scratch2/", listing, f"no mount point at /\n{listing!r}")

        self._run("cd /scratch2")
        self._run("mkdir madehere")
        listing = self._run("ls")
        self.assertIn(b"madehere", listing, f"not writable\n{listing!r}")
        self._run("cd /")
        listing = self._run("ls")
        self.assertNotIn(
            b"madehere", listing, f"the root and /scratch2 share state\n{listing!r}"
        )

    def test_a_path_that_is_already_a_mount_is_refused(self):
        """Mounts do not stack: two filesystems at one path would make routing
        pick between them by registration order, with no way to name the covered
        one."""
        self._run("cd /")
        out = self._run("mount -t tmpfs /", timeout_s=40)
        self.assertIn(b"fs.MOUNT_EXISTS", out, f"stacked on the root\n{out!r}")
        out = self._run("mount -t tmpfs /boot", timeout_s=40)
        self.assertIn(b"fs.MOUNT_EXISTS", out, f"stacked on /boot\n{out!r}")

    def test_an_unknown_type_and_a_mismatched_source_are_refused(self):
        """MOUNT_FSTYPE covers both: a type with no driver, and a type whose
        device requirement the request does not match. A source given for tmpfs
        is refused rather than ignored, because ignoring it would answer a
        different request than the one made."""
        self._run("cd /")
        out = self._run("mount -t nosuchfs /nope", timeout_s=40)
        self.assertIn(b"fs.MOUNT_FSTYPE", out, f"unknown type accepted\n{out!r}")
        out = self._run("mount -t tmpfs /nope2 block:ata:0", timeout_s=40)
        self.assertIn(
            b"fs.MOUNT_FSTYPE", out, f"a source for tmpfs was ignored\n{out!r}"
        )
        out = self._run("mount -t wfs /nope3", timeout_s=40)
        self.assertIn(b"fs.MOUNT_FSTYPE", out, f"wfs mounted with no source\n{out!r}")
        out = self._run("mount")
        for absent in (b"/nope ->", b"/nope2 ->", b"/nope3 ->"):
            self.assertNotIn(absent, out, f"a refused mount was created\n{out!r}")

    def test_a_runtime_mount_can_be_unmounted_again(self):
        """The two halves meet: what this request created, UNMOUNT removes, and
        the mount point it left behind is still a directory."""
        self._run("cd /")
        out = self._run("mount -t tmpfs /roundtrip", timeout_s=40)
        self.assertIn(b"mounted", out, f"mount refused\n{out!r}")
        out = self._run("umount /roundtrip")
        self.assertIn(b"unmounted", out, f"unmount refused\n{out!r}")
        out = self._run("mount")
        self.assertNotIn(b"/roundtrip ->", out, f"still in the table\n{out!r}")
        listing = self._run("ls")
        self.assertIn(b"roundtrip/", listing, f"the mount point went too\n{listing!r}")


class VfsUnmountTest(VfsSession, unittest.TestCase):
    """Removing a mount, and refusing to remove one that is still in use.

    Its own session, because unmounting is destructive to the namespace every
    other class here reads: a mount removed by one case must not be missing when
    another asserts it is present.

    Cases run in unittest's alphabetical order and are NOT independent -- the
    refusals must be observed while `/wfs/nested` still stands, and the two
    removals must happen inside out. The method names carry that order, which is
    why the last one begins with `test_with_`.

    Regression: 2026-09-01-unmount-completes-shadowing
    Mounts could be established but never removed, so shadowing was only ever
    demonstrated in one direction: content disappeared under a mount and had no
    way to come back. A covered file that never reappears is indistinguishable
    from a deleted one.
    """

    def test_a_mount_with_a_deeper_mount_inside_it_is_refused(self):
        """`/wfs` contains the `/wfs/nested` mount. Removing the outer one would
        leave the inner reachable only through a prefix that no longer routes.

        `/` is refused for the same reason and not a special case: every other
        mount is inside it.
        """
        self._run("cd /")
        out = self._run("umount /wfs")
        self.assertIn(b"fs.MOUNT_BUSY", out, f"the deeper mount was ignored\n{out!r}")
        out = self._run("umount /")
        self.assertIn(b"fs.MOUNT_BUSY", out, f"the root was removable\n{out!r}")
        out = self._run("mount")
        self.assertIn(b"/wfs ->", out, f"a refused unmount still removed it\n{out!r}")
        self.assertIn(
            b"/ ->", out, f"a refused unmount still removed the root\n{out!r}"
        )

    def test_a_path_that_is_not_a_mount_is_not_unmountable(self):
        """`/home` exists -- fs-manager created it walking to `/home/user` -- but
        it is a directory, not a mount, and NO_BACKEND says exactly that."""
        self._run("cd /")
        out = self._run("umount /home")
        self.assertIn(b"fs.NO_BACKEND", out, f"a plain directory unmounted\n{out!r}")
        out = self._run("umount /nowhere")
        self.assertIn(b"fs.NO_BACKEND", out, f"a missing path unmounted\n{out!r}")

    def test_the_mount_table_is_read_from_the_service_not_the_shell(self):
        """`mount` is a spawned utility, not a shell builtin.

        It asks fs-manager for the table (FSMGR_IPC_QUERY_MOUNTS_REQ) and prints
        the reply, so what it shows is what routing uses. A listing the shell
        assembled could disagree with the service that owns it.
        """
        self._run("cd /")
        out = self._run("mount")
        self.assertIn(b"mounts:", out, f"no table printed\n{out!r}")
        self.assertIn(b"/ ->", out, f"root missing from the table\n{out!r}")

    def test_unmounting_uncovers_what_the_volume_holds(self):
        """The other half of shadowing.

        `/wfs/nested/covered.txt` is a file in the WFS image that the tmpfs
        mounted over `/wfs/nested` hides. Removing that mount must make it
        readable again -- the mount hid it, so nothing about the volume changed
        and the file was never gone.

        The mount point itself stays: it is a directory in the WFS volume, owned
        by that volume rather than by the mount.
        """
        self._run("cd /wfs/nested")
        listing = self._run("ls")
        self.assertNotIn(
            b"covered.txt", listing, f"not shadowed to begin with\n{listing!r}"
        )

        # Standing elsewhere: the covered file is read back through a fresh
        # lookup rather than through a directory handle opened before the mount
        # went away.
        self._run("cd /")
        out = self._run("umount /wfs/nested")
        self.assertIn(b"unmounted", out, f"unmount refused\n{out!r}")

        out = self._run("mount")
        self.assertNotIn(b"/wfs/nested ->", out, f"still in the mount table\n{out!r}")

        self._run("cd /wfs/nested")
        listing = self._run("ls")
        self.assertIn(
            b"covered.txt",
            listing,
            f"the covered file did not come back\n{listing!r}",
        )
        out = self._run("cat covered.txt")
        self.assertIn(
            b"exists only to be covered",
            out,
            f"uncovered but not readable\n{out!r}",
        )

    def test_with_the_inner_mount_gone_the_outer_one_is_removable(self):
        """Busy is a condition, not a property: what made `/wfs` unremovable was
        the mount inside it, so removing that one releases it.

        Runs last -- the cases above read `/wfs`, and this removes it.
        """
        self._run("cd /")
        self._run("umount /wfs/nested")
        out = self._run("umount /wfs")
        self.assertIn(b"unmounted", out, f"still refused\n{out!r}")
        out = self._run("mount")
        self.assertNotIn(b"/wfs ->", out, f"still in the mount table\n{out!r}")
        # The mount POINT survives its mount: it is a directory in the root
        # filesystem, created when the mount registered and not owned by it.
        listing = self._run("ls")
        self.assertIn(b"wfs/", listing, f"the mount point was removed too\n{listing!r}")


class VfsRootWritableTest(VfsSession, unittest.TestCase):
    """The root filesystem accepts writes, exercised through the `mkdir` util.

    Its own session, not a shared one: creating a directory at `/` changes what
    `ls /` reports, and VfsRootMountTest asserts that the root listing is EXACTLY
    the mount points. Sharing a session would make one case decide the other's
    outcome depending on the order unittest happened to pick.
    """

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

    def test_mkdir_p_treats_a_leading_slash_run_as_one_slash(self):
        """`//x/y` names the same directory as `/x/y`.

        Regression: 2026-08-31-mkdir-p-leading-slash-run — mkdir_parents skipped
        ONE leading slash and then read the next as a segment terminator, so the
        first component it tried to create was "/" itself. fs-manager refuses
        that, and the refusal is not EXISTS, so the whole -p failed. `mkdir -p /`
        failed the same way, where the root always existing should make it a
        trivial success.
        """
        self._run("cd /")
        out = self._run("mkdir -p //runx/runy")
        self.assertNotIn(b"mkdir:", out, f"a leading slash run failed\n{out!r}")
        out = self._run("cd /runx/runy")
        self.assertIn(b"/runx/runy wamos>", out, f"path not created\n{out!r}")

        # The root is nobody's to create, so asking for it succeeds trivially.
        self._run("cd /")
        out = self._run("mkdir -p /")
        self.assertNotIn(b"mkdir:", out, f"`mkdir -p /` reported an error\n{out!r}")

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


class VfsRootListingWithoutChdirTest(VfsSession, unittest.TestCase):
    """`ls` works as the FIRST filesystem command, with no `cd` before it.

    The session's settle wait sends `mount`, which does NOT prime that cache:
    fs-manager answers FSMGR_IPC_QUERY_MOUNTS_REQ before it looks up client state,
    so no state exists until the `ls` below. If that order ever changes, this case
    stops testing anything and should be made to fail rather than pass quietly.

    Regression: 2026-08-31-readdir-without-chdir — fs-manager cached the backend
    serving a client's working directory on CHDIR, and READDIR forwarded to that
    cache. A client that never chdir'd still held the initial -1, so `ls` at the
    root answered "request could not be delivered to the backend, or no reply
    arrived" (WASMOS_ERR_FS_BACKEND_IPC).

    It was invisible until `/` became a mount: before that, READDIR at
    FS_MOUNT_ROOT was short-circuited into the virtual mount-table listing and
    never reached the forward path. Every other case in this file happens to run
    `cd` first, which primes the cache -- which is why this one needs its own
    session and must not send `cd` at all.
    """

    def test_ls_at_the_root_before_any_chdir(self):
        listing = self._run("ls")
        self.assertNotIn(
            b"fs failed",
            listing,
            f"`ls` failed as the first command\n{listing!r}",
        )
        # And it is the real listing, not an empty success.
        self.assertIn(b"boot", listing, f"`ls` produced no root entries\n{listing!r}")


if __name__ == "__main__":
    unittest.main()
