import subprocess
import struct
import tempfile
import unittest
from pathlib import Path

from scripts.qemu_test_framework import default_host_tool_path

# Packed .wap header prefix, in the field order of wasmos_app_header_t in
# scripts/make_wasmos_app.c, up to but excluding subsystem_tag: magic, version,
# header_size, flags, name_len, entry_len, wasm_size, dep_count, cap_count,
# mem_hint_count, compiled_count.
#
# The tag must be located by OFFSET, never from the end of the header:
# region_count follows the tag, so "the last 8 bytes" reads region_count plus
# padding and comes back as zeros.
#
# The prefix is spelled out field by field rather than counted, because the
# format is not append-only -- fields have been removed from the middle, which
# shifts everything after them. A layout change requires revising this string.
_HEADER_PREFIX = "<8sHHIIIIIIII"
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


_DEP_NEEDS, _DEP_WANTS, _DEP_PROVIDES = 1, 2, 3
_DEP_KIND_NAME, _DEP_KIND_CLASS = 1, 2


def _manifest(name: str, deps: str = "") -> str:
    return "\n".join(
        [
            "[package]",
            f'name = "{name}"',
            'entry = "initialize"',
            'kind = "service"',
            "",
            "[resources]",
            "stack_pages = 1",
            "heap_pages = 1",
            "",
            deps,
        ]
    )


def _dep_records(wap: bytes) -> list:
    """Decode the dependency table of a packed .wap (name and entry follow the
    header; the table follows them)."""
    header_size = struct.unpack_from("<H", wap, 10)[0]
    name_len, entry_len, _wasm_size, dep_count = struct.unpack_from("<IIII", wap, 16)
    off = header_size + name_len + entry_len
    out = []
    for _ in range(dep_count):
        strength, kind, length = struct.unpack_from("<BBH", wap, off)
        off += 4
        out.append((strength, kind, wap[off : off + length].decode()))
        off += length
    return out


class MakeWasmosAppDepsTest(unittest.TestCase):
    def setUp(self):
        self.packer = Path(default_host_tool_path("make_wasmos_app"))
        self.assertTrue(self.packer.exists(), f"{self.packer} must exist")
        self._td = tempfile.TemporaryDirectory()
        self.td = Path(self._td.name)
        self.wasm = self.td / "dummy.wasm"
        self.wasm.write_bytes(b"\x00asm")

    def tearDown(self):
        self._td.cleanup()

    def _pack(self, text: str):
        manifest = self.td / "m.toml"
        manifest.write_text(text, encoding="utf-8")
        out = self.td / "m.wap"
        result = subprocess.run(
            [
                str(self.packer),
                "--manifest",
                str(manifest),
                "--in",
                str(self.wasm),
                "--out",
                str(out),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        return result, out

    def _check(self, manifests: dict):
        paths = []
        for name, text in manifests.items():
            path = self.td / f"{name}.toml"
            path.write_text(text, encoding="utf-8")
            paths.append(str(path))
        return subprocess.run(
            [str(self.packer), "--check-deps", *paths],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_deps_are_packed_in_declaration_order(self):
        result, out = self._pack(
            _manifest(
                "svc",
                "\n".join(
                    [
                        "[deps]",
                        'needs = ["fs.vfs", "class:block"]',
                        'wants = ["font"]',
                        'provides = ["gfx"]',
                    ]
                ),
            )
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            _dep_records(out.read_bytes()),
            [
                (_DEP_NEEDS, _DEP_KIND_NAME, "fs.vfs"),
                (_DEP_NEEDS, _DEP_KIND_CLASS, "block"),
                (_DEP_WANTS, _DEP_KIND_NAME, "font"),
                (_DEP_PROVIDES, _DEP_KIND_NAME, "gfx"),
            ],
        )

    def test_no_deps_table_packs_no_records(self):
        result, out = self._pack(_manifest("svc"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(_dep_records(out.read_bytes()), [])

    def test_ipc_table_is_refused(self):
        result, _ = self._pack(
            _manifest("svc", '[ipc]\nrequired_endpoint_name = "proc"\n')
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("[deps]", result.stderr)

    def test_invalid_deps_are_refused(self):
        for deps, reason in (
            ('[deps]\nneeds = ["Bad"]', "outside"),
            ('[deps]\nneeds = ["class:abcdefghijklmnop"]', "longer"),
            ('[deps]\nneeds = [""]', "empty"),
            ('[deps]\nneeds = ["vt", "vt"]', "duplicate"),
            ('[deps]\nneeds = "vt"', "array"),
            ('[deps]\nrequires = ["vt"]', "unknown"),
            (
                "[deps]\nwants = [" + ", ".join(f'"s{i}"' for i in range(17)) + "]",
                "more than",
            ),
        ):
            with self.subTest(deps=deps):
                result, _ = self._pack(_manifest("svc", deps))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(reason, result.stderr)

    def test_check_deps_rejects_a_needs_cycle(self):
        result = self._check(
            {
                "a": _manifest("a", '[deps]\nneeds = ["b"]\nprovides = ["a"]'),
                "b": _manifest("b", '[deps]\nneeds = ["class:c"]\nprovides = ["b"]'),
                "c": _manifest("c", '[deps]\nneeds = ["a"]\nprovides = ["class:c"]'),
            }
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dependency cycle", result.stderr)

    def test_check_deps_accepts_soft_cycles_and_self_provided_classes(self):
        result = self._check(
            {
                # a and b want each other: soft edges never form a cycle.
                "a": _manifest("a", '[deps]\nwants = ["b"]\nprovides = ["a"]'),
                "b": _manifest(
                    "b", '[deps]\nneeds = ["a"]\nwants = ["a"]\nprovides = ["b"]'
                ),
                # A partition driver provides and needs the same class.
                "p": _manifest(
                    "p", '[deps]\nneeds = ["class:block"]\nprovides = ["class:block"]'
                ),
                "d": _manifest("d", '[deps]\nprovides = ["class:block"]'),
            }
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_check_deps_notes_a_need_nobody_provides(self):
        result = self._check({"a": _manifest("a", '[deps]\nneeds = ["ghost"]')})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ghost", result.stderr)


if __name__ == "__main__":
    unittest.main()
