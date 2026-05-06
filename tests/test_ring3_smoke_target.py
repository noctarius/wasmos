import os
import subprocess
import unittest


class Ring3SmokeTargetTests(unittest.TestCase):
    def test_run_qemu_ring3_target(self) -> None:
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        cmd = ["cmake", "--build", "build", "--target", "run-qemu-ring3-test"]
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
