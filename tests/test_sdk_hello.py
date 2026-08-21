"""The staged SDK's zero-configuration path, end to end in the guest, per driver.

examples/c/sdk_hello/hello.c is plain C with no linker.metadata, built by
cmake/WasmosSdk.cmake with the staged driver and no flags but -o. That link line
is not the in-tree one -- it goes through the sysroot, crt1.o and libc.a rather
than recompiling libc into the module -- so the in-tree build being green says
nothing about it. This is the test that says the SDK produces a program WASMOS
actually runs.

Three claims, in increasing depth, because a shallower one passing says little
about a deeper one:
  - console output works (crt1 + libc + the console host call);
  - argv is real, with argv[1] the first argument -- crt1 tokenizes the
    spawn-info argument string, and the off-by-one that puts the first argument
    in argv[0] would still print something plausible;
  - open/read reach the filesystem SERVICE over IPC, which is where a missing
    archive object or a bad sysroot header surfaces at runtime rather than at
    the link step.
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

# The SDK target is skipped when llvm-ar is absent (cmake/WasmosSdk.cmake returns
# early), and then the app never reaches the ESP. Skip rather than fail: the
# absence is a toolchain gap on the host, not a regression in the guest.
BUILD_DIR = os.environ.get("WASMOS_BUILD_DIR", os.path.join(ROOT, "build"))
SDK_HELLO_WAP = os.path.join(BUILD_DIR, "esp", "apps", "sdkhello.wap")
SDK_ZIG_WAP = os.path.join(BUILD_DIR, "esp", "apps", "sdkzig.wap")
SDK_AS_WAP = os.path.join(BUILD_DIR, "esp", "apps", "sdkas.wap")
SDK_RUST_WAP = os.path.join(BUILD_DIR, "esp", "apps", "sdkrust.wap")
SDK_GO_WAP = os.path.join(BUILD_DIR, "esp", "apps", "sdkgo.wap")


@unittest.skipUnless(
    os.path.isfile(SDK_HELLO_WAP),
    "SDK smoke app not staged on the ESP (needs llvm-ar and the wasmos-sdk target)",
)
class SdkHelloTest(unittest.TestCase):
    """One boot, one case per driver. The non-C cases skip on their own when their
    compiler is absent, rather than living in subclasses that would boot the guest
    again to run a single case each."""

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

    def _cmd_expect(self, cmd: str, needles: list[bytes], timeout_s: int = 30) -> None:
        mark = self.session.mark()
        self.session.send(cmd)
        for needle in needles:
            if not self.session.expect_from(mark, needle, timeout_s=timeout_s):
                self.fail(
                    f"Expected output not found for '{cmd}': {needle!r}\n"
                    f"--- tail ---\n{self.session.tail()}\n"
                )
        if not self.session.expect_from(mark, b"wamos> ", timeout_s=timeout_s):
            self.fail(
                f"Prompt not found after '{cmd}'.\n--- tail ---\n{self.session.tail()}\n"
            )

    def test_exec_sdk_hello(self):
        self._cmd_expect("cd apps", [b"/apps wamos>"])
        self._cmd_expect(
            "sdkhello",
            [
                b"Hello WASMOS from the SDK!",
                # No argument: argc is 1, the program-name slot alone.
                b"argc=1",
                b"fs: read ",
            ],
        )

    def test_sdk_hello_receives_argv(self):
        """An argument reaches argv[1], not argv[0].

        The path is echoed back by the program, so a tokenizer that shifted the
        arguments down would print argc=1 and the default path instead.
        """
        self._cmd_expect("cd apps", [b"/apps wamos>"])
        self._cmd_expect(
            "sdkhello /boot/system/net/interfaces",
            [
                b"argc=2 argv[1]=/boot/system/net/interfaces",
                b"fs: read ",
            ],
        )

    @unittest.skipUnless(
        os.path.isfile(SDK_ZIG_WAP),
        "SDK Zig smoke app not staged (needs zig and the wasmos-sdk target)",
    )
    def test_exec_sdk_zig_hello(self):
        """wasmos-zig hides more than the C driver, and none of it is visible in
        the source: the runtime shims are staged flat so @import("wasmos.zig")
        resolves, and the 8 KiB shadow stack is mandatory because Zig's 1 MB
        default puts the app's globals past the kernel's user-VA mirror region,
        where host calls that write to WASM memory fail SILENTLY. Running the
        module is the only way to know the driver got both right."""
        self._cmd_expect("cd apps", [b"/apps wamos>"])
        self._cmd_expect("sdkzig", [b"Hello WASMOS from Zig via the SDK!"])

    @unittest.skipUnless(
        os.path.isfile(SDK_AS_WAP),
        "SDK AssemblyScript smoke app not staged (needs asc and the wasmos-sdk target)",
    )
    def test_exec_sdk_assemblyscript_hello(self):
        """asc has no include path and resolves imports relative to the entry, so
        wasmos-asc stages the whole AS runtime flat beside a copy of the app --
        under the name runtime.ts imports, not the developer's filename. Whether
        that staging produced a module the runtime can instantiate is only
        answerable by running it."""
        self._cmd_expect("cd apps", [b"/apps wamos>"])
        self._cmd_expect("sdkas", [b"Hello WASMOS from AssemblyScript via the SDK!"])

    @unittest.skipUnless(
        os.path.isfile(SDK_RUST_WAP),
        "SDK Rust smoke app not staged (needs rustc and the wasmos-sdk target)",
    )
    def test_exec_sdk_rust_hello(self):
        """rustc defaults to a 1 MB shadow stack with --stack-first, which puts the
        app's data above 1 MB -- past the user-VA mirror region host calls
        validate against. wasmos-rustc overrides it to the same small size the Zig
        driver uses and checks the resulting layout, and the binding is staged as a
        sibling module so a plain `mod wasmos;` resolves. Running the module is what
        says both worked."""
        self._cmd_expect("cd apps", [b"/apps wamos>"])
        self._cmd_expect("sdkrust", [b"Hello WASMOS from Rust via the SDK!"])

    @unittest.skipUnless(
        os.path.isfile(SDK_GO_WAP),
        "SDK Go smoke app not staged (needs tinygo, wasm-opt and the wasmos-sdk target)",
    )
    def test_exec_sdk_go_hello(self):
        """TinyGo is configured by a target FILE whose extra-files are resolved
        relative to TINYGOROOT, so wasmos-tinygo generates that file per invocation
        after asking tinygo where its root is, with the C shims' paths computed
        against it. Nothing about that is checkable short of running the module."""
        self._cmd_expect("cd apps", [b"/apps wamos>"])
        self._cmd_expect("sdkgo", [b"Hello WASMOS from Go via the SDK!"])


if __name__ == "__main__":
    unittest.main()
