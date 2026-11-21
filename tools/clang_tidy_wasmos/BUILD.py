"""The wasmos clang-tidy plugin: the wasmos-* checks `lint` loads."""

import shlex
import subprocess


def build(b):
    tools = b.tools
    if not (tools.get("clang-tidy") and tools.get("llvm-config")):
        return
    cxxflags = shlex.split(
        subprocess.run(
            [tools["llvm-config"], "--cxxflags"], capture_output=True, text=True
        ).stdout
    )
    if tools.get("clang-tidy-include"):
        cxxflags.append(f"-I{tools['clang-tidy-include']}")
    darwin = tools["host-os"] == "Darwin"
    plugin = "wasmos_tidy_plugin" + (".dylib" if darwin else ".so")
    # Symbols resolve against the clang-tidy binary that loads the plugin.
    undefined = (
        ["-undefined", "dynamic_lookup"] if darwin else ["-Wl,--allow-shlib-undefined"]
    )
    src = b.src("WasmosTidyModule.cpp")
    b.index(src, [tools["clangxx"], *cxxflags, "-fPIC"])
    b.command(
        [plugin],
        [
            tools["clangxx"],
            *cxxflags,
            "-fPIC",
            "-shared",
            *undefined,
            src,
            "-o",
            plugin,
        ],
        inputs=[src],
        description=f"LINK[host] {plugin}",
    )
    b.exports["tidy_plugin"] = plugin
    b.alias("wasmos_tidy_plugin", plugin)
