import subprocess
import tempfile
import unittest
from pathlib import Path


class MakeWasmosAppCapabilitiesTest(unittest.TestCase):
    def test_rejects_unknown_and_flagged_capability(self):
        packer = Path("build/make_wasmos_app")
        self.assertTrue(packer.exists(), "build/make_wasmos_app must exist")

        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            wasm_path = td_path / "dummy.wasm"
            out_path = td_path / "dummy.wap"
            wasm_path.write_bytes(b"\x00asm")

            base = [
                str(packer),
                str(wasm_path),
                str(out_path),
                "cap-test",
                "wasmos_main",
                "1",
                "1",
                "4",
                "-",
                "0",
            ]

            unknown = subprocess.run(
                base + ["1", "no.such.cap", "0"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(unknown.returncode, 0)
            self.assertIn("unknown capability", unknown.stderr)

            bad_flags = subprocess.run(
                base + ["1", "io.port", "1"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(bad_flags.returncode, 0)
            self.assertIn("unsupported capability flags", bad_flags.stderr)


if __name__ == "__main__":
    unittest.main()
