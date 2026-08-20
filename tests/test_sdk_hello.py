"""The staged SDK's zero-configuration path, end to end in the guest.

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
SDK_HELLO_WAP = os.path.join(
    os.environ.get("WASMOS_BUILD_DIR", os.path.join(ROOT, "build")),
    "esp",
    "apps",
    "sdkhello.wap",
)


@unittest.skipUnless(
    os.path.isfile(SDK_HELLO_WAP),
    "SDK smoke app not staged on the ESP (needs llvm-ar and the wasmos-sdk target)",
)
class SdkHelloTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
