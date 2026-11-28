import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from qemu_test_framework import QemuSession, build_config_enabled, default_config

AOT_USED = "[warp-driver] using AOT binary"
AOT_FAILED = "[warp-driver] AOT load failed"


@unittest.skipUnless(
    build_config_enabled("WASMOS_WASM_RUNTIME_WARP")
    and build_config_enabled("WASMOS_WARP_RUNTIME_LOAD_AOT"),
    "the tree does not load WARP AOT binaries",
)
class WarpAotLoadTest(unittest.TestCase):
    """A module packed with an AOT binary runs from that binary.

    The build compiles every `aot = true` module ahead of time with warp_aot
    and bundles the code in its .wap; a kernel built with
    WASMOS_WARP_RUNTIME_LOAD_AOT loads it instead of compiling at spawn. A
    binary the kernel cannot load falls back to the JIT with one log line and
    the system otherwise behaves the same, so the fallback is asserted absent.
    WARP does not check the ISA of a compiled binary: code for another
    architecture loads and then executes as garbage.

    Regression: 2026-11-28-warp-aot-host-isa -- warp_aot was built with
    JIT_TARGET_X86_64 whatever the target, so aarch64 trees packed x86_64 code
    and had to boot with AOT loading off, compiling every module at spawn.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=180, echo=True)
        cls.session.start()
        cls.session.require_prompt()
        if not cls.session.settle():
            tail = cls.session.tail()
            cls.session.close()
            raise RuntimeError(
                f"system never went idle after boot\n--- tail ---\n{tail}\n"
            )

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.send("halt")
            cls.session.close()

    def test_boot_modules_load_their_aot_binaries(self) -> None:
        log = self.session.syslog_tail(1 << 20)
        self.assertNotIn(
            AOT_FAILED,
            log,
            f"a bundled AOT binary did not load\n--- kernel log ---\n{log[-4096:]}",
        )
        self.assertGreater(
            log.count(AOT_USED),
            0,
            f"no module loaded an AOT binary\n--- kernel log ---\n{log[-4096:]}",
        )

    def test_an_aot_utility_runs(self) -> None:
        start = self.session.send_command("sysinfo", timeout_s=30)
        self.assertTrue(
            self.session.expect_from(start, b"processes", timeout_s=30),
            f"sysinfo (AOT) did not run\n--- tail ---\n{self.session.tail()}",
        )
        log = self.session.syslog_tail(1 << 20)
        self.assertNotIn(AOT_FAILED, log, f"--- kernel log ---\n{log[-4096:]}")


if __name__ == "__main__":
    unittest.main()
