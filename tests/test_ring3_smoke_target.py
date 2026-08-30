import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from scripts.qemu_test_framework import default_build_dir


class Ring3SmokeTargetTests(unittest.TestCase):
    def test_run_qemu_ring3_target(self) -> None:
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        # Resolved, not hardcoded to "build". A tree configured under any other
        # directory -- a defconfig build, a worktree -- would otherwise fail on a
        # missing path and report it as a ring3 regression, which is the same
        # defect default_kernel_path and default_host_tool_path already fixed for
        # the kernel and the host tools.
        build_dir = os.path.relpath(default_build_dir(), root)
        cmd = ["cmake", "--build", build_dir, "--target", "run-qemu-ring3-test"]
        proc = subprocess.run(
            cmd,
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            tail = "\n".join(proc.stdout.splitlines()[-120:])
            self.fail(
                "run-qemu-ring3-test failed with exit code "
                f"{proc.returncode}\n--- output tail ---\n{tail}"
            )


if __name__ == "__main__":
    unittest.main()
