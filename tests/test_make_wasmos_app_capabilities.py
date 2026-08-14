import subprocess
import struct
import tempfile
import unittest
from pathlib import Path

from scripts.qemu_test_framework import default_host_tool_path

# Packed .wap header prefix, in the field order of wasmos_app_header_t in
# scripts/make_wasmos_app.c, up to but excluding subsystem_tag: magic, version,
# header_size, flags, name_len, entry_len, wasm_size, req_ep_count, cap_count,
# mem_hint_count, the four driver_match u8s, the four driver match/io u16s,
# driver_match_count, compiled_size.
#
# The tag must be located by OFFSET, never from the end of the header -- v6 added
# region_count after the tag, so "the last 8 bytes" silently became region_count
# plus padding and read back as zeros.
#
# Every version up to v6 extended the format by APPENDING, which kept older
# offsets stable. v7 is the first that does not: it REMOVED
# entry_arg_binding_count from the middle, shifting subsystem_tag and
# region_count down by four bytes. Any reader that hardcodes an offset past that
# field has to be revised per version rather than written once -- which is why
# this prefix is spelled out field by field instead of being counted from the end.
_HEADER_PREFIX = "<8sHHIIIIIIIBBBBHHHHII"
_SUBSYSTEM_TAG_OFFSET = struct.calcsize(_HEADER_PREFIX)
_SUBSYSTEM_TAG_LEN = 8


def _subsystem_tag(header: bytes) -> bytes:
    """Read the subsystem tag out of a packed .wap header."""
    end = _SUBSYSTEM_TAG_OFFSET + _SUBSYSTEM_TAG_LEN
    header_size = struct.unpack_from("<H", header, 10)[0]
    assert (
        header_size >= end
    ), f"header_size {header_size} cannot hold the subsystem tag"
    return header[_SUBSYSTEM_TAG_OFFSET:end].rstrip(b"\0")


class MakeWasmosAppCapabilitiesTest(unittest.TestCase):
    def test_rejects_unknown_and_flagged_capability(self):
        packer = Path(default_host_tool_path("make_wasmos_app"))
        self.assertTrue(packer.exists(), f"{packer} must exist")

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
        packer = Path(default_host_tool_path("make_wasmos_app"))
        self.assertTrue(packer.exists(), f"{packer} must exist")

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
            self.assertEqual(_subsystem_tag(default_hdr), b"WASM")
            self.assertEqual(_subsystem_tag(explicit_hdr), b"WARP")


if __name__ == "__main__":
    unittest.main()
