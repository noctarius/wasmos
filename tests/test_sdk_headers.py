"""Every public header in the sysroot compiles.

The SDK's smoke apps use stdio, fcntl and unistd, and passed while the sysroot was
missing `wasmos_driver_abi.h` entirely -- a header that `wasmos/ipc.h`,
`wasmos/proc.h`, `wasmos/net.h` and `wasmos/libsys.h` all include by bare name. So
the SDK could build hello-world and could not build anything that talks to a
service, and nothing noticed, because no smoke app included those headers.

This compiles each header on its own. Doing them one at a time rather than in one
translation unit is deliberate: a header that only works when something else was
included first is a header a developer will trip over, and a single combined probe
would hide exactly that.

It is a host-tools test: compiling is the whole check, so it needs no QEMU.
"""

import os
import subprocess
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.environ.get("WASMOS_BUILD_DIR", os.path.join(ROOT, "build"))
SDK_DIR = os.path.join(BUILD_DIR, "wasmos-sdk")
WASMOS_CLANG = os.path.join(SDK_DIR, "bin", "wasmos-clang")
INCLUDE_DIR = os.path.join(SDK_DIR, "sysroot", "include")

# Headers that are FRAGMENTS by design: each is included by an umbrella header
# after that header has established what the fragment needs, and is not meant to be
# included directly. Each entry carries the umbrella that owns it, so an exception
# can be checked rather than trusted -- and the umbrellas themselves
# (wasmos/libui.h, wasmos/api.h) are compiled by the test like everything else,
# which is what keeps this list from becoming a place to hide a real breakage.
#
# Anything NOT listed here that fails to compile is a defect in the sysroot.
NOT_STANDALONE = {
    # api.h includes this last, after the struct typedefs some declarations take by
    # reference and the WASMOS_WASM_IMPORT macro they are annotated with.
    os.path.join("wasmos", "abi", "wasmos_imports.h"): "wasmos/api.h",
    # libui.h defines the component model and the shared drawing state, then
    # includes each component at the end of the file (line 1841+).
    **{
        os.path.join("wasmos", f"libui_{part}.h"): "wasmos/libui.h"
        for part in (
            "button",
            "checkbox",
            "dropdown",
            "label",
            "list_view",
            "menu_bar",
            "menu_item",
            "row",
            "scroll_view",
            "text_input",
            "tree_view",
        )
    },
}


def public_headers() -> list[str]:
    """Every .h under the sysroot include root, as an #include-able path."""
    found = []
    for dirpath, _dirs, files in os.walk(INCLUDE_DIR):
        rel_dir = os.path.relpath(dirpath, INCLUDE_DIR)
        for name in sorted(files):
            if not name.endswith(".h"):
                continue
            rel = name if rel_dir == "." else os.path.join(rel_dir, name)
            found.append(rel)
    return sorted(found)


@unittest.skipUnless(
    os.access(WASMOS_CLANG, os.X_OK),
    "SDK not staged (cmake --build build --target wasmos-sdk)",
)
class SdkHeadersTest(unittest.TestCase):
    def test_fragment_umbrellas_exist_and_compile(self):
        """Each fragment's umbrella is real and compiles.

        Without this the exception list above could quietly excuse a header whose
        umbrella was itself broken or gone.
        """
        umbrellas = sorted(set(NOT_STANDALONE.values()))
        for umbrella in umbrellas:
            path = os.path.join(INCLUDE_DIR, umbrella)
            self.assertTrue(os.path.isfile(path), f"missing umbrella: {umbrella}")
            result = subprocess.run(
                [WASMOS_CLANG, "-c", "-x", "c", "-", "-o", os.devnull],
                input=f'#include "{umbrella}"\nint probe;\n',
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(
                result.returncode, 0, f"{umbrella} does not compile:\n{result.stderr}"
            )

    def test_headers_are_present(self):
        headers = public_headers()
        self.assertGreater(len(headers), 20, f"suspiciously few headers: {headers}")
        # The one whose absence started this test, named so a regression is obvious.
        self.assertIn("wasmos_driver_abi.h", headers)
        self.assertIn(os.path.join("wasmos", "ipc.h"), headers)

    def test_every_header_compiles_alone(self):
        failures = []
        for header in public_headers():
            if header in NOT_STANDALONE:
                continue
            source = f'#include "{header}"\nint probe_{abs(hash(header)) % 10**8};\n'
            result = subprocess.run(
                [WASMOS_CLANG, "-c", "-x", "c", "-", "-o", os.devnull],
                input=source,
                capture_output=True,
                text=True,
                timeout=120,
            )
            if result.returncode != 0:
                failures.append(f"{header}:\n{result.stderr.strip()}")
        self.assertEqual(
            failures,
            [],
            "headers that do not compile on their own:\n\n" + "\n\n".join(failures),
        )


if __name__ == "__main__":
    unittest.main()
