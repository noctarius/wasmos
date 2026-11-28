"""warp_aot-<arch>: host tools that compile a wasm module ahead of time with
WARP's code generator for one target architecture, whatever the host's. Built
only for a WARP configuration."""

import platform

from toolchains import ARCHES, WARP_CODEGEN_DEFINES, arch

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

# WARP sources that integrate with the HOST operating system (signals), and so
# keep the host's OS macros; every other WARP source is code generation and is
# compiled as the kernel compiles it (target_os_neutral.h).
HOST_OS_SOURCES = {"src/utils/SignalFunctionWrapper_unix.cpp"}


def _host_arch():
    machine = platform.machine().lower()
    return {"amd64": "x86_64", "arm64": "aarch64"}.get(machine, machine)


def build(b):
    warp = b.root / "libs/warp"
    common = [
        "-std=c++14",
        "-O2",
        *WARP_CODEGEN_DEFINES,
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
    tree_arch = arch(b.cfg).name
    neutral = ["-include", str(b.src("target_os_neutral.h"))]
    host = b.toolchain("host")
    if not b.cfg.WASMOS_WASM_RUNTIME_WARP:
        # Only a WARP tree builds the tool; index its own sources regardless.
        for src in b.srcs(*TOOL_SOURCES):
            b.index(src, [host.tc.cxx, *host.tc.cxxflags, *common, arch(b.cfg).warp_jit_target])
        return
    # One tool per target architecture, warp_aot-<arch>, with that
    # architecture's code generator (Arch.warp_jit_target). WARP records no ISA
    # in a compiled binary, so the packer tags each .warpbin with the
    # architecture of the tool that made it (--compiled <arch>=<path>), and the
    # kernel uses only its own. A tree's own modules use its own architecture's
    # tool; a package with `aot = "all"` (wasmos_rules.pack) also uses the
    # others, and only then are they built.
    codegen = [s for s in WARP_SOURCES if s not in HOST_OS_SOURCES]
    tools = {}
    for name, spec in ARCHES.items():
        flags = [*common, spec.warp_jit_target]
        objs = host.compile(
            [warp / s for s in codegen],
            cxxflags=flags + neutral,
            subdir=f"obj/warp_aot/{name}",
        )
        objs += host.compile(
            [warp / s for s in sorted(HOST_OS_SOURCES)] + b.srcs(*TOOL_SOURCES),
            cxxflags=flags,
            subdir=f"obj/warp_aot/{name}",
        )
        tool = f"warp_aot-{name}"
        b.command(
            [tool],
            [b.tools["clangxx"], *objs, "-o", tool],
            inputs=objs,
            description=f"LINK[host] {tool}",
        )
        tools[name] = tool
    b.exports["warp_aot"] = {"tree": tree_arch, "tools": tools}
    # The WARP host test (tests/unit) links the same runtime and EXECUTES what it
    # compiles, so it is built for the host's own architecture and keeps the
    # host's OS macros: its code must follow the host's calling convention.
    host_arch = _host_arch()
    host_target = ARCHES[host_arch].warp_jit_target if host_arch in ARCHES else ""
    b.exports["warp_host"] = {
        "sources": [warp / s for s in WARP_SOURCES]
        + b.srcs("host_mem_utils.cpp", "host_shim.cpp"),
        "flags": [*common, host_target] if host_target else common,
    }
    b.alias("warp_aot_tool", tools[tree_arch])
