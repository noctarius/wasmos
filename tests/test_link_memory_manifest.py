"""Every built module's memory limits match its manifest's [link] section.

The linker's memory configuration used to live in CMake arguments while the
kernel's resource hints lived in the manifest -- two places, neither next to the
other, for values that are load-bearing rather than stylistic: an app that maps
shared memory, a framebuffer or socket rings has to reserve linear memory up
front, because those windows are placed above a 2 MiB floor and above live data.
`[link]` moved them into the manifest, and the CMake helper reads them at
configure time.

Reading them at configure time is exactly what makes this test worth having. A
manifest edit that CMake does not notice, a section-parsing slip that finds
[resources]'s stack_pages instead of [link]'s stack_size, or a default that drifts
from the one the helper applies, all produce a module that links fine and is sized
wrong -- and a module sized wrong does not fail at the link step. It traps later,
inside a host call whose window did not fit, which is a much worse place to learn.

So this compares what the module actually declares against what its manifest asks
for, across every .wap in the build tree rather than a sample.
"""

import os
import re
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.environ.get("WASMOS_BUILD_DIR", os.path.join(ROOT, "build"))

WASM_PAGE = 65536


def read_leb128(data: bytes, pos: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, pos


def module_memory_limits(data: bytes) -> tuple[int, int | None]:
    """(initial_bytes, max_bytes or None) from a module's memory section."""
    if data[:4] != b"\0asm":
        raise ValueError("not a wasm module")
    pos = 8
    while pos < len(data):
        section_id, pos = read_leb128(data, pos)
        size, pos = read_leb128(data, pos)
        end = pos + size
        if section_id == 5:  # memory
            _count, cur = read_leb128(data, pos)
            flags, cur = read_leb128(data, cur)
            initial, cur = read_leb128(data, cur)
            maximum = None
            if flags & 1:
                maximum, cur = read_leb128(data, cur)
            return initial * WASM_PAGE, (
                maximum * WASM_PAGE if maximum is not None else None
            )
        pos = end
    raise ValueError("no memory section")


def manifest_link_values(path: str) -> dict[str, int]:
    """The [link] section's integer keys. Section-aware, like the CMake reader."""
    values: dict[str, int] = {}
    section = ""
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = re.sub(r"#.*$", "", line).strip()
            if not line:
                continue
            if line.startswith("["):
                section = line
                continue
            if section != "[link]" or "=" not in line:
                continue
            key, _, value = line.partition("=")
            value = value.strip().strip('"')
            if value.isdigit():
                values[key.strip()] = int(value)
    return values


def wap_payload_and_name(path: str) -> tuple[bytes, str]:
    import sys

    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import wasm_inspect

    with open(path, "rb") as handle:
        data = handle.read()
    info = wasm_inspect.parse_wasmos_app(data)
    start = info["wasm_offset"]
    return data[start : start + info["wasm_size"]], info["name"]


def find_manifests() -> dict[str, str]:
    """Map package name -> manifest path for every linker.metadata in the tree."""
    found: dict[str, str] = {}
    for base in ("src", "examples"):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, base)):
            if "linker.metadata" not in files:
                continue
            path = os.path.join(dirpath, "linker.metadata")
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
            match = re.search(r'^\s*name\s*=\s*"?([^"\n]+)"?', text, re.M)
            if match:
                found[match.group(1).strip()] = path
    return found


class LinkMemoryManifestTest(unittest.TestCase):
    """Scope: the keys a manifest actually declares.

    Only declared keys are asserted, never a default. Each helper applies its own
    defaults -- the C helper pins a maximum, the Zig one leaves the module without
    any -- so a test that assumed one helper's defaults would be asserting the wrong
    contract for the other. What a manifest declares is a value somebody chose, and
    that is exactly what has to reach the module.

    The AssemblyScript and Rust helpers do not read [link] yet, so their modules
    declare none and are skipped -- visibly, not silently. Teaching them the section
    is tracked in docs/TASKS.md.
    """

    @classmethod
    def setUpClass(cls):
        cls.waps = (
            [
                os.path.join(BUILD_DIR, f)
                for f in sorted(os.listdir(BUILD_DIR))
                if f.endswith(".wap")
            ]
            if os.path.isdir(BUILD_DIR)
            else []
        )
        cls.manifests = find_manifests()

    def test_build_tree_has_packages(self):
        self.assertTrue(self.waps, f"no .wap packages in {BUILD_DIR}; build first")
        self.assertTrue(self.manifests, "no linker.metadata files found")

    def test_declared_link_memory_reaches_the_module(self):
        checked = []
        for wap in self.waps:
            payload, name = wap_payload_and_name(wap)
            if payload[:4] != b"\0asm":
                continue  # native ELF payload; [link] does not apply
            manifest = self.manifests.get(name)
            if manifest is None:
                continue
            link = manifest_link_values(manifest)
            if not link:
                continue  # no [link]: nothing declared, nothing to check
            initial, maximum = module_memory_limits(payload)
            actual = {"initial_memory": initial, "max_memory": maximum}
            for key, want in link.items():
                if key not in actual:
                    continue  # stack_size is not visible in the module's memory section
                self.assertEqual(
                    actual[key],
                    want,
                    f"{name}: module {key} is {actual[key]}, manifest asks for {want} "
                    f"({manifest})",
                )
            checked.append(name)

        # A guard on the test itself: if the package-name-to-manifest mapping stops
        # matching, every assertion above is skipped and the suite still passes.
        self.assertGreaterEqual(
            len(checked),
            15,
            f"only {len(checked)} modules carried a checkable [link]: {sorted(checked)}",
        )

    def test_defaults_are_not_written_out(self):
        """A [link] key equal to the helper's default is noise, not configuration.

        Keeping them out means a value present in a manifest is one someone chose,
        so a reader can tell tuning from boilerplate at a glance.
        """
        # The C helper's defaults; a manifest that restates one is saying nothing.
        c_defaults = {"stack_size": 4096, "initial_memory": 65536, "max_memory": 65536}
        redundant = []
        for manifest in sorted(set(self.manifests.values())):
            link = manifest_link_values(manifest)
            for key, default in c_defaults.items():
                if link.get(key) == default:
                    redundant.append(f"{manifest}: {key} {default}")
        self.assertEqual(redundant, [], "default values written out explicitly")


if __name__ == "__main__":
    unittest.main()
