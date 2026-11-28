"""warp_aot-<arch>: host tools that compile a wasm module ahead of time with
WARP's code generator for one target architecture, whatever the host's. Built
only for a WARP configuration."""

import platform

# Vendored WARP sources (libs/warp is a git subtree; never edited here).
WARP_SOURCES = [
    "src/WasmModule/WasmModule.cpp",
    "src/core/runtime/Runtime.cpp",
    "src/core/runtime/ActiveMemoryManager.cpp",
    "src/core/runtime/MemoryHelper.cpp",
    "src/core/common/BinaryModule.cpp",
    "src/core/common/VbExceptions.cpp",
    "src/core/common/ExtendableMemory.cpp",
    "src/utils/VirtualMemoryAllocator.cpp",
    "src/utils/LinearMemoryAllocator.cpp",
    "src/utils/OSAPIChecker.cpp",
    "src/utils/SignalFunctionWrapper.cpp",
    "src/utils/SignalFunctionWrapper_unix.cpp",
    "src/core/compiler/Compiler.cpp",
    "src/core/compiler/backend/BackendBase.cpp",
    "src/core/compiler/backend/x86_64/x86_64_assembler.cpp",
    "src/core/compiler/backend/x86_64/x86_64_backend.cpp",
    "src/core/compiler/backend/x86_64/x86_64_call_dispatch.cpp",
    "src/core/compiler/backend/x86_64/x86_64_cc.cpp",
    "src/core/compiler/backend/x86_64/x86_64_instruction.cpp",
    "src/core/compiler/backend/x86_64/x86_64_relpatchobj.cpp",
    "src/core/compiler/backend/aarch64/aarch64_assembler.cpp",
    "src/core/compiler/backend/aarch64/aarch64_aux.cpp",
    "src/core/compiler/backend/aarch64/aarch64_backend.cpp",
    "src/core/compiler/backend/aarch64/aarch64_call_dispatch.cpp",
    "src/core/compiler/backend/aarch64/aarch64_cc.cpp",
    "src/core/compiler/backend/aarch64/aarch64_instruction.cpp",
    "src/core/compiler/backend/aarch64/aarch64_relpatchobj.cpp",
    "src/core/compiler/backend/tricore/tricore_assembler.cpp",
    "src/core/compiler/backend/tricore/tricore_aux.cpp",
    "src/core/compiler/backend/tricore/tricore_backend.cpp",
    "src/core/compiler/backend/tricore/tricore_call_dispatch.cpp",
    "src/core/compiler/backend/tricore/tricore_cc.cpp",
    "src/core/compiler/backend/tricore/tricore_instruction.cpp",
    "src/core/compiler/backend/tricore/tricore_relpatchobj.cpp",
    "src/core/compiler/common/Common.cpp",
    "src/core/compiler/common/CommonRegsPos.cpp",
    "src/core/compiler/common/MemWriter.cpp",
    "src/core/compiler/common/ModuleInfo.cpp",
    "src/core/compiler/common/Stack.cpp",
    "src/core/compiler/frontend/BytecodeReader.cpp",
    "src/core/compiler/frontend/Frontend.cpp",
    "src/core/compiler/frontend/ValidationStack.cpp",
]
TOOL_SOURCES = ["host_mem_utils.cpp", "host_shim.cpp", "warp_aot.cpp"]


# The JIT target each architecture's tool is built with. WARP picks its code
# generator at compile time and records no ISA in a compiled binary, so a
# .warpbin is tagged with its architecture by the packer (--compiled
# <arch>=<path>), and the kernel uses only the one for its own.
JIT_TARGETS = {"x86_64": "-DJIT_TARGET_X86_64", "aarch64": "-DJIT_TARGET_AARCH64"}


def _host_arch():
    machine = platform.machine().lower()
    return {"amd64": "x86_64", "arm64": "aarch64"}.get(machine, machine)


def build(b):
    warp = b.root / "libs/warp"
    flags = [
        "-std=c++14",
        "-O2",
        "-DACTIVE_STACK_OVERFLOW_CHECK=1",
        "-DLINEAR_MEMORY_BOUNDS_CHECKS=1",
        "-DVB_POSIX",
        "-DVB_WIN32_OR_POSIX",
        "-DNVALGRIND",
        f"-I{warp}",
        f"-I{warp / 'src'}",
        f"-I{b.root / 'abi/generated/c'}",
        "-Wno-deprecated",
        "-Wno-unused-variable",
        "-Wno-macro-redefined",
        "-Wno-c++20-extensions",
        "-Wno-deprecated-copy",
        "-Wno-unused-parameter",
        "-Wno-unsequenced",
        "-Wno-#pragma-messages",
    ]
    tree_arch = "aarch64" if b.cfg.WASMOS_ARCH_AARCH64 else "x86_64"
    host = b.toolchain("host")
    if not b.cfg.WASMOS_WASM_RUNTIME_WARP:
        # Only a WARP tree builds the tool; index its own sources regardless.
        for src in b.srcs(*TOOL_SOURCES):
            b.index(
                src, [host.tc.cxx, *host.tc.cxxflags, *flags, JIT_TARGETS[tree_arch]]
            )
        return
    # One tool per target architecture, warp_aot-<arch>. A tree's own modules use
    # its own architecture's; a package with `aot = "all"` (wasmos_rules.pack)
    # also uses the others, and only then are they built.
    tools = {}
    for arch, target in JIT_TARGETS.items():
        objs = host.compile(
            [warp / s for s in WARP_SOURCES] + b.srcs(*TOOL_SOURCES),
            cxxflags=[*flags, target],
            subdir=f"obj/warp_aot/{arch}",
        )
        tool = f"warp_aot-{arch}"
        b.command(
            [tool],
            [b.tools["clangxx"], *objs, "-o", tool],
            inputs=objs,
            description=f"LINK[host] {tool}",
        )
        tools[arch] = tool
    b.exports["warp_aot"] = {"tree": tree_arch, "tools": tools}
    # The WARP host test (tests/unit) links the same runtime and EXECUTES what it
    # compiles, so its target is the host's architecture, not the tree's.
    host_target = JIT_TARGETS.get(_host_arch(), JIT_TARGETS["x86_64"])
    b.exports["warp_host"] = {
        "sources": [warp / s for s in WARP_SOURCES]
        + b.srcs("host_mem_utils.cpp", "host_shim.cpp"),
        "flags": [*flags, host_target],
    }
    b.alias("warp_aot_tool", tools[tree_arch])
