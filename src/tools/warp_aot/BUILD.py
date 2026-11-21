"""warp_aot: host tool that compiles a wasm module ahead of time with WARP
(x86_64 backend). Built only for a WARP configuration."""

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


def build(b):
    if not b.cfg.WASMOS_WASM_RUNTIME_WARP:
        return
    warp = b.root / "libs/warp"
    flags = [
        "-std=c++14",
        "-O2",
        "-DJIT_TARGET_X86_64",
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
    host = b.toolchain("host")
    objs = host.compile(
        [warp / s for s in WARP_SOURCES] + b.srcs(*TOOL_SOURCES),
        cxxflags=flags,
        subdir="obj/warp_aot",
    )
    tool = "warp_aot"
    b.command(
        [tool],
        [b.tools["clangxx"], *objs, "-o", tool],
        inputs=objs,
        description=f"LINK[host] {tool}",
    )
    b.exports["warp_aot"] = tool
    b.alias("warp_aot_tool", tool)
