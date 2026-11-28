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

AOT_USED = "[warp-driver] using AOT binary for "
AOT_FAILED = "[warp-driver] AOT load failed for "
NO_AOT_HERE = "[warp-driver] no AOT binary for this architecture, compiling "
UNTRUSTED = (
    "[warp-driver] AOT binary of a package not signed by a trusted key, compiling "
)
PROBE_OK = b"[test] aot-probe ok sum=5050"
REFUSED = "[wasmos-app] refused, signed but verified by no trusted key: "


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

    Regression: 2026-11-28-wap-aot-untagged -- a .wap did not record which
    architecture its AOT binary was for, so a package built for x86_64 and
    spawned on aarch64 executed x86_64 code at EL0 and panicked the kernel. A
    v12 package tags each binary with its ELF machine and may carry one per
    architecture; the kernel uses only its own and otherwise compiles the
    bytecode. examples/c/aot_probe is packed both ways.

    Regression: 2026-11-28-aot-unsigned -- the kernel ran any package's AOT
    binary as native code, so an unsigned package could carry a hand-made one
    tagged for this machine and skip every check WARP's compiler emits. Only a
    package signed by a trusted key has its binary used; examples/c/aot_probe
    is also packed unsigned.
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

    def _spawn_probe(self, path: str) -> str:
        start = self.session.send_command(f"spawn {path}", timeout_s=30)
        self.assertTrue(
            self.session.expect_from(start, PROBE_OK, timeout_s=30),
            f"{path} did not run\n--- tail ---\n{self.session.tail()}",
        )
        return self.session.syslog_tail(1 << 20)

    def test_a_fat_package_runs_from_its_own_architecture(self) -> None:
        log = self._spawn_probe("/boot/apps/aotfat")
        self.assertIn(
            AOT_USED + "aot-fat",
            log,
            f"the fat package did not use its AOT binary\n--- kernel log ---\n{log[-4096:]}",
        )
        self.assertNotIn(NO_AOT_HERE + "aot-fat", log)
        self.assertNotIn(AOT_FAILED + "aot-fat", log)

    def test_a_package_without_this_architecture_runs_from_bytecode(self) -> None:
        log = self._spawn_probe("/boot/apps/aotforgn")
        self.assertIn(
            NO_AOT_HERE + "aot-foreign",
            log,
            f"no fallback was reported\n--- kernel log ---\n{log[-4096:]}",
        )
        self.assertNotIn(AOT_USED + "aot-foreign", log)

    def test_an_unsigned_package_runs_from_bytecode(self) -> None:
        log = self._spawn_probe("/boot/apps/aotunsig")
        self.assertIn(
            UNTRUSTED + "aot-unsigned",
            log,
            f"no fallback was reported\n--- kernel log ---\n{log[-4096:]}",
        )
        self.assertNotIn(AOT_USED + "aot-unsigned", log)

    def test_a_package_signed_by_an_unknown_key_does_not_start(self) -> None:
        """Regression: 2026-11-28-signed-invalid-starts -- a package carrying a
        signature no trusted key verifies (altered, or signed with a key this
        kernel was not given) still started, merely unprivileged. It is now
        refused before the process is created."""
        start = self.session.send_command("spawn /boot/apps/aotuntru", timeout_s=30)
        self.assertTrue(
            self.session.expect_from(start, b"wamos> ", timeout_s=30),
            f"no prompt after the refused spawn\n--- tail ---\n{self.session.tail()}",
        )
        self.assertNotIn(PROBE_OK, bytes(self.session.buf[start:]))
        log = self.session.syslog_tail(1 << 20)
        self.assertIn(
            REFUSED + "aot-untrusted",
            log,
            f"the refusal was not logged\n--- kernel log ---\n{log[-4096:]}",
        )

    def test_an_aot_utility_runs(self) -> None:
        start = self.session.send_command("sysinfo", timeout_s=30)
        self.assertTrue(
            self.session.expect_from(start, b"processes", timeout_s=30),
            f"sysinfo (AOT) did not run\n--- tail ---\n{self.session.tail()}",
        )
        log = self.session.syslog_tail(1 << 20)
        self.assertNotIn(
            AOT_FAILED + "sysinfo", log, f"--- kernel log ---\n{log[-4096:]}"
        )


if __name__ == "__main__":
    unittest.main()
