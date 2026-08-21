"""ABI shape of what the SDK links, asserted without booting anything.

A WASMOS module's import list is its dependency on the kernel, and the SDK must
not widen it silently: an import outside the declared ABI means either a stub that
drifted from abi/hostcalls.yaml or a runtime dependency (WASI, an accidental host
symbol) that the linker was allowed to invent. The C toolchain does not need
--allow-undefined, so an undeclared symbol is a link error rather than an import;
this test is what notices if that stops being true.

Runs against the artifact cmake/WasmosSdk.cmake already built, and skips when the
SDK has not been staged, so it belongs to the host-tools battery and needs no QEMU.
"""

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCRIPTS = os.path.join(ROOT, "scripts")
for path in (ROOT, SCRIPTS):
    if path not in sys.path:
        sys.path.insert(0, path)

import wasm_inspect

BUILD_DIR = os.environ.get("WASMOS_BUILD_DIR", os.path.join(ROOT, "build"))
SDK_DIR = os.path.join(BUILD_DIR, "wasmos-sdk")
SDK_HELLO_WAP = os.path.join(BUILD_DIR, "sdkhello.wap")
HOSTCALLS_YAML = os.path.join(ROOT, "abi", "hostcalls.yaml")

# Imports a WASMOS module may carry beyond the wasmos.* host calls. env.abort is
# the AssemblyScript abort hook; the two wasi_snapshot_preview1 names are the
# deliberate WARP-only entries in abi/hostcalls.yaml. Nothing else is legitimate,
# and in particular a C module built by the SDK should reach none of them.
EXTRA_ALLOWED_IMPORTS = frozenset(
    {
        "env.abort",
        "env.strlen",
        "wasi_snapshot_preview1.proc_exit",
        "wasi_snapshot_preview1.random_get",
    }
)


def declared_hostcall_names() -> set[str]:
    """Host-call names in abi/hostcalls.yaml, read without a YAML dependency.

    An entry is either `  - name: x` or the one-line flow mapping
    `  - { name: x, ... }`; both forms appear in the IDL. The two-space indent is
    part of the match and not incidental: a parameter is written the same way at a
    deeper indent, and folding parameter names into this set would let an import
    called `len` or `flags` pass as a declared host call. A name is accepted
    regardless of its module, because the module is checked separately against
    EXTRA_ALLOWED_IMPORTS.
    """
    names = set()
    with open(HOSTCALLS_YAML, encoding="utf-8") as handle:
        for line in handle:
            if not line.startswith("  - "):
                continue
            body = line[4:].strip()
            if body.startswith("{"):
                body = body[1:].split(",", 1)[0].strip()
            if not body.startswith("name:"):
                continue
            names.add(body.split(":", 1)[1].strip().rstrip("}").strip())
    return names


def read_module(path: str) -> tuple[dict | None, dict]:
    """Return (container info or None, parsed wasm) for a .wap or bare .wasm."""
    with open(path, "rb") as handle:
        data = handle.read()
    app_info = None
    if data.startswith(wasm_inspect.WASMOS_MAGIC):
        app_info = wasm_inspect.parse_wasmos_app(data)
        start = app_info["wasm_offset"]
        data = data[start : start + app_info["wasm_size"]]
    return app_info, wasm_inspect.parse_wasm(data)


def module_imports(path: str) -> list[str]:
    _app_info, wasm = read_module(path)
    return [f"{module}.{name}" for module, name, _kind in wasm["imports"]]


@unittest.skipUnless(
    os.path.isfile(SDK_HELLO_WAP),
    "SDK smoke app not built (cmake --build build --target sdk_hello_app)",
)
class SdkAbiTest(unittest.TestCase):
    def test_imports_are_declared_hostcalls(self):
        declared = declared_hostcall_names()
        self.assertIn(
            "console_write", declared, "hostcall IDL parse produced nothing usable"
        )
        for imported in module_imports(SDK_HELLO_WAP):
            module, _, name = imported.partition(".")
            if imported in EXTRA_ALLOWED_IMPORTS:
                continue
            self.assertEqual(
                module,
                "wasmos",
                f"{imported} is outside the WASMOS ABI (module {module!r})",
            )
            self.assertIn(
                name,
                declared,
                f"{imported} is not declared in abi/hostcalls.yaml",
            )

    def test_no_wasi_imports(self):
        for imported in module_imports(SDK_HELLO_WAP):
            self.assertFalse(
                imported.startswith("wasi_snapshot_preview1."),
                f"C module built by the SDK reached WASI: {imported}",
            )

    def test_exports_are_memory_and_entry(self):
        app_info, wasm = read_module(SDK_HELLO_WAP)
        self.assertIsNotNone(app_info, "SDK smoke app is not a .wap container")
        self.assertEqual(app_info["entry"], "wasmos_main")
        names = sorted(name for name, _kind, _idx in wasm["exports"])
        self.assertEqual(names, ["memory", "wasmos_main"])

    def test_sysroot_headers_carry_no_repo_relative_abi_include(self):
        """api.h's in-tree include reaches outside src/; a sysroot cannot.

        cmake/wasmos_sdk_stage.cmake rewrites it and fails loudly if it cannot, so
        this asserts the installed result rather than the rewrite.
        """
        api = os.path.join(SDK_DIR, "sysroot", "include", "wasmos", "api.h")
        self.assertTrue(os.path.isfile(api), f"sysroot api.h missing: {api}")
        with open(api, encoding="utf-8") as handle:
            text = handle.read()
        self.assertNotIn("../../../../abi/generated", text)
        self.assertIn("wasmos/abi/wasmos_imports.h", text)


if __name__ == "__main__":
    unittest.main()
