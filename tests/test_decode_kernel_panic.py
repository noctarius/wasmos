import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


class DecodeKernelPanicScriptTest(unittest.TestCase):
    def test_decodes_rip_and_backtrace_frames(self):
        script = Path("scripts/decode_kernel_panic.py")
        self.assertTrue(script.exists(), "decode_kernel_panic.py must exist")

        panic_log = textwrap.dedent(
            """\
            [cpu] rip=ffffffff80201bb4

            ================= KERNEL PANIC =================
            reason : cpu_exception
            cpus=1  panicking_cpu=0
            --- CPU 0 captured=1 pid=0 tid=0 ---
                rip=ffffffff80201bb4 rsp=ffffffff8036ffb0 rbp=ffffffff8036ffe0 rflags=0000000000010046
                backtrace:
                [0] ret=ffffffff80201ba9 (panic_verify_level2)
                [1] ret=ffffffff80201b79 (panic_verify_level1)
                [2] ret=ffffffff80201922 (kmain)
            """
        )

        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            kernel = td_path / "kernel.elf"
            kernel.write_bytes(b"not-an-elf-but-present")

            fake_addr2line = td_path / "fake-addr2line.py"
            fake_addr2line.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import sys

                    mapping = {
                        "0xffffffff80201bb4": "panic_verify_level3 at src/kernel/kernel.c:45",
                        "0xffffffff80201ba9": "panic_verify_level2 at src/kernel/kernel.c:51",
                        "0xffffffff80201b79": "panic_verify_level1 at src/kernel/kernel.c:57",
                        "0xffffffff80201922": "kmain at src/kernel/kernel.c:349",
                    }
                    for addr in sys.argv[6:]:
                        print(mapping[addr])
                    """
                ),
                encoding="utf-8",
            )
            fake_addr2line.chmod(
                fake_addr2line.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(script),
                    "--kernel",
                    str(kernel),
                    "--addr2line",
                    str(fake_addr2line),
                ],
                input=panic_log,
                text=True,
                capture_output=True,
                check=False,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(
                "cpu0 rip 0xffffffff80201bb4 -> panic_verify_level3 at src/kernel/kernel.c:45",
                result.stdout,
            )
            self.assertIn(
                "cpu0 frame[0] 0xffffffff80201ba9 -> panic_verify_level2 at src/kernel/kernel.c:51",
                result.stdout,
            )
            self.assertIn(
                "cpu0 frame[2] 0xffffffff80201922 -> kmain at src/kernel/kernel.c:349",
                result.stdout,
            )

    def test_discovers_addr2line_from_cmake_cache(self):
        script = Path("scripts/decode_kernel_panic.py")
        self.assertTrue(script.exists(), "decode_kernel_panic.py must exist")

        panic_log = "rip=ffffffff80201bb4\n"

        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            llvm_bin = td_path / "llvm-bin"
            llvm_bin.mkdir()

            kernel = td_path / "build" / "kernel.elf"
            kernel.parent.mkdir()
            kernel.write_bytes(b"present")
            (kernel.parent / "CMakeCache.txt").write_text(
                f"CLANG:STRING={llvm_bin / 'clang'}\n",
                encoding="utf-8",
            )
            (llvm_bin / "clang").write_text("", encoding="utf-8")

            fake_addr2line = llvm_bin / "llvm-addr2line"
            fake_addr2line.write_text(
                textwrap.dedent(
                    """\
                    #!/bin/sh
                    echo "panic_verify_level3 at src/kernel/kernel.c:45"
                    """
                ),
                encoding="utf-8",
            )
            fake_addr2line.chmod(
                fake_addr2line.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(script),
                    "--kernel",
                    str(kernel),
                ],
                input=panic_log,
                text=True,
                capture_output=True,
                check=False,
                env={**os.environ, "PATH": "", "PYTHONDONTWRITEBYTECODE": "1"},
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(
                "cpu? rip 0xffffffff80201bb4 -> panic_verify_level3 at src/kernel/kernel.c:45",
                result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
