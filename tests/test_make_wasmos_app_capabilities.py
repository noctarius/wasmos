import subprocess
import struct
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

    def test_writes_default_and_explicit_subsystem_tags(self):
        packer = Path("build/make_wasmos_app")
        self.assertTrue(packer.exists(), "build/make_wasmos_app must exist")

        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            wasm_path = td_path / "dummy.wasm"
            default_manifest = td_path / "default.toml"
            explicit_manifest = td_path / "explicit.toml"
            default_out = td_path / "default.wap"
            explicit_out = td_path / "explicit.wap"
            wasm_path.write_bytes(b"\x00asm")
            default_manifest.write_text(
                "\n".join(
                    [
                        "[package]",
                        'name = "tag-default"',
                        'entry = "wasmos_main"',
                        'kind = "app"',
                        "native = false",
                        "",
                        "[resources]",
                        "stack_pages = 1",
                        "heap_pages = 1",
                        "",
                        "[ipc]",
                        'required_endpoint_name = "-"',
                        "required_endpoint_rights = 0",
                    ]
                ),
                encoding="utf-8",
            )
            explicit_manifest.write_text(
                default_manifest.read_text(encoding="utf-8").replace(
                    'kind = "app"\n',
                    'kind = "app"\nsubsystem = "WARP"\n',
                ),
                encoding="utf-8",
            )

            for manifest, out_path in (
                (default_manifest, default_out),
                (explicit_manifest, explicit_out),
            ):
                result = subprocess.run(
                    [
                        str(packer),
                        "--manifest",
                        str(manifest),
                        "--in",
                        str(wasm_path),
                        "--out",
                        str(out_path),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

            default_hdr = default_out.read_bytes()
            explicit_hdr = explicit_out.read_bytes()
            default_header_size = struct.unpack_from("<H", default_hdr, 10)[0]
            explicit_header_size = struct.unpack_from("<H", explicit_hdr, 10)[0]
            self.assertEqual(
                default_hdr[default_header_size - 8 : default_header_size].rstrip(
                    b"\0"
                ),
                b"WASM",
            )
            self.assertEqual(
                explicit_hdr[explicit_header_size - 8 : explicit_header_size].rstrip(
                    b"\0"
                ),
                b"WARP",
            )


if __name__ == "__main__":
    unittest.main()
