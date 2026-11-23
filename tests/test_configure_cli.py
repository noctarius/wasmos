"""configure's command line and the source-root compile_commands.json link.

Host-only: loads ./configure as a module and exercises its argument parsing and
update_ide_link() against a temporary source root. Nothing is generated or
built.
"""

import importlib.machinery
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_configure():
    loader = importlib.machinery.SourceFileLoader(
        "wasmos_configure", str(ROOT / "configure")
    )
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


configure = load_configure()


class ConfigurePositionalsTest(unittest.TestCase):
    def setUp(self):
        self._cwd = os.getcwd()
        os.chdir(ROOT)

    def tearDown(self):
        os.chdir(self._cwd)

    def test_two_positionals_are_builddir_and_defconfig(self):
        _, args = configure.parse_args(["out/x", "configs/x86_64_warp_smp_defconfig"])
        self.assertEqual(args.builddir, Path("out/x"))
        self.assertEqual(args.defconfig, Path("configs/x86_64_warp_smp_defconfig"))

    def test_lone_defconfig_configures_default_builddir(self):
        _, args = configure.parse_args(["configs/x86_64_warp_smp_defconfig"])
        self.assertEqual(args.builddir, configure.DEFAULT_BUILDDIR)
        self.assertEqual(args.defconfig, Path("configs/x86_64_warp_smp_defconfig"))

    def test_lone_directory_is_builddir(self):
        _, args = configure.parse_args(["out/x"])
        self.assertEqual(args.builddir, Path("out/x"))
        self.assertIsNone(args.defconfig)

    def test_no_positionals_configures_default_builddir(self):
        _, args = configure.parse_args([])
        self.assertEqual(args.builddir, configure.DEFAULT_BUILDDIR)
        self.assertIsNone(args.defconfig)

    def test_option_between_positionals(self):
        # Regression: 2026-11-22-configure-intermixed-positionals
        # `./configure <dir> --reseed <defconfig>` was accepted before the build
        # directory became optional; afterwards the defconfig after the option
        # was rejected as an unrecognized argument, so documented reseed
        # invocations stopped working.
        _, args = configure.parse_args(
            ["out/x", "--reseed", "configs/x86_64_warp_smp_defconfig"]
        )
        self.assertEqual(args.builddir, Path("out/x"))
        self.assertEqual(args.defconfig, Path("configs/x86_64_warp_smp_defconfig"))
        self.assertTrue(args.reseed)

    def test_existing_builddir_named_like_a_defconfig(self):
        # Regression: 2026-11-22-configure-defconfig-named-builddir
        # A lone existing build directory whose name ends in "defconfig" was
        # taken for a defconfig. build.ninja's regeneration command passes the
        # build directory alone, so such a tree regenerated out/default instead
        # of itself.
        with tempfile.TemporaryDirectory() as tmp:
            builddir = Path(tmp) / "custom_defconfig"
            builddir.mkdir()
            _, args = configure.parse_args(["--regen", str(builddir)])
        self.assertEqual(args.builddir, builddir)
        self.assertIsNone(args.defconfig)


class IdeLinkTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.src = Path(self._tmp.name).resolve()
        self._srcdir = configure.SRCDIR
        configure.SRCDIR = self.src
        self.link = self.src / "compile_commands.json"
        for name in ("a", "b"):
            (self.src / "out" / name).mkdir(parents=True)
            (self.src / "out" / name / "compile_commands.json").write_text("[]\n")

    def tearDown(self):
        configure.SRCDIR = self._srcdir
        self._tmp.cleanup()

    def target(self):
        return Path(os.readlink(self.link))

    def test_auto_creates_missing_link_relative_to_srcdir(self):
        configure.update_ide_link(self.src / "out/a", "auto")
        self.assertEqual(self.target(), Path("out/a/compile_commands.json"))

    def test_auto_keeps_a_valid_link(self):
        configure.update_ide_link(self.src / "out/a", "auto")
        configure.update_ide_link(self.src / "out/b", "auto")
        self.assertEqual(self.target(), Path("out/a/compile_commands.json"))

    def test_auto_replaces_a_dangling_link(self):
        self.link.symlink_to("out/gone/compile_commands.json")
        configure.update_ide_link(self.src / "out/b", "auto")
        self.assertEqual(self.target(), Path("out/b/compile_commands.json"))

    def test_force_repoints(self):
        configure.update_ide_link(self.src / "out/a", "auto")
        configure.update_ide_link(self.src / "out/b", "force")
        self.assertEqual(self.target(), Path("out/b/compile_commands.json"))

    def test_off_leaves_the_link_alone(self):
        configure.update_ide_link(self.src / "out/a", "off")
        self.assertFalse(self.link.is_symlink())

    def test_regular_file_is_never_replaced(self):
        self.link.write_text("[]\n")
        configure.update_ide_link(self.src / "out/a", "force")
        self.assertFalse(self.link.is_symlink())
        self.assertEqual(self.link.read_text(), "[]\n")

    def test_out_of_tree_builddir_gets_an_absolute_link(self):
        with tempfile.TemporaryDirectory() as ext:
            outdir = Path(ext).resolve()
            configure.update_ide_link(outdir, "force")
            self.assertEqual(self.target(), outdir / "compile_commands.json")

    def test_concurrent_configure_temporary_link_is_not_touched(self):
        # Regression: 2026-11-22-configure-shared-link-tmp
        # Every configure staged the link through the same temporary name. A
        # second configure racing the first unlinked the first one's staged
        # link, which then failed in replace() with FileNotFoundError after its
        # build had been generated.
        staged = self.src / "compile_commands.json.tmp"
        staged.symlink_to("out/a/compile_commands.json")
        configure.update_ide_link(self.src / "out/b", "auto")
        self.assertTrue(staged.is_symlink(), "another configure's staged link")
        self.assertEqual(self.target(), Path("out/b/compile_commands.json"))
        self.assertEqual(
            sorted(p.name for p in self.src.iterdir()),
            ["compile_commands.json", "compile_commands.json.tmp", "out"],
        )


if __name__ == "__main__":
    unittest.main()
