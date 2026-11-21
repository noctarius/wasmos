"""Toolchain discovery and per-target compile profiles.

A profile is one target class -- UEFI loader, kernel, ring-3 native probe,
wasm32 guest, host tool -- with its compiler, the flags every object of that
class shares, and its linker. Manifests pick a profile by name and add only
component-specific flags. A new architecture is a new set of profiles here.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

LLVM_HINTS = [
    "/opt/homebrew/opt/llvm/bin",
    "/usr/local/opt/llvm/bin",
    "/opt/homebrew/bin",
    "/usr/local/bin",
]
LLD_HINTS = ["/opt/homebrew/opt/lld/bin", "/usr/local/opt/lld/bin"]


def _find(name: str, hints: list[str], required=True) -> str | None:
    for d in hints:
        p = Path(d) / name
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    p = shutil.which(name)
    if p:
        return p
    if required:
        raise SystemExit(
            f"buildgen: {name} not found (searched {', '.join(hints)} and PATH)"
        )
    return None


def discover(overrides: dict[str, str], srcdir: Path) -> dict[str, str]:
    """Resolve every external tool once, at configure time. The result is
    stored in the build directory, so regeneration never re-probes PATH and a
    build directory keeps the toolchain it was configured with."""
    tools: dict[str, str] = {}
    clang = overrides.get("clang") or _find("clang", LLVM_HINTS)
    version = subprocess.run(
        [clang, "--version"], capture_output=True, text=True
    ).stdout
    if "Apple clang" in version:
        raise SystemExit(
            "buildgen: AppleClang cannot build UEFI targets. Install LLVM clang "
            "and pass --clang=/path/to/llvm/bin/clang."
        )
    bindir = str(Path(clang).parent)
    hints = [bindir] + LLVM_HINTS
    tools["clang"] = clang
    tools["clangxx"] = overrides.get("clangxx") or _find("clang++", hints)
    tools["ld.lld"] = overrides.get("ld.lld") or _find(
        "ld.lld", [bindir] + LLD_HINTS + LLVM_HINTS
    )
    tools["lld-link"] = overrides.get("lld-link") or _find(
        "lld-link", [bindir] + LLD_HINTS + LLVM_HINTS
    )
    for t in (
        "llvm-objcopy",
        "llvm-nm",
        "llvm-ar",
        "llvm-ranlib",
        "llvm-strip",
        "llvm-objdump",
    ):
        tools[t] = overrides.get(t) or _find(t, hints)
    tools["wasm-ld"] = (
        overrides.get("wasm-ld")
        or _find("wasm-ld", [bindir] + LLD_HINTS + LLVM_HINTS, required=False)
        or ""
    )
    tools["clang-resource-dir"] = subprocess.run(
        [tools["clangxx"], "-print-resource-dir"], capture_output=True, text=True
    ).stdout.strip()

    # Guest-language toolchains are optional: the SDK records whatever exists
    # and its wrappers report a missing one when invoked.
    home = os.path.expanduser("~")
    tools["zig"] = (
        overrides.get("zig")
        or _find("zig", [bindir, f"{home}/bin", f"{home}/.local/bin"], required=False)
        or ""
    )
    tools["asc"] = overrides.get("asc") or _find("asc", [bindir], required=False) or ""
    tools["rustc"] = (
        overrides.get("rustc")
        or _find("rustc", [f"{home}/.cargo/bin"], required=False)
        or ""
    )
    tools["tinygo"] = (
        overrides.get("tinygo") or _find("tinygo", [], required=False) or ""
    )
    tools["wasm-opt"] = (
        overrides.get("wasm-opt")
        or _find("wasm-opt", [str(srcdir / "node_modules/.bin")], required=False)
        or ""
    )
    tools["host-os"] = platform.system()
    return tools


@dataclass
class Toolchain:
    name: str
    cc: str
    cxx: str
    ld: str
    ar: str
    cflags: list[str] = field(default_factory=list)
    cxxflags: list[str] = field(default_factory=list)
    asflags: list[str] = field(default_factory=list)
    ldflags: list[str] = field(default_factory=list)


def profiles(srcdir: Path, cfg, tools: dict) -> dict[str, Toolchain]:
    clang, clangxx = tools["clang"], tools["clangxx"]
    libc_inc = srcdir / "src/libc/include"
    cast_h = str(libc_inc / "wasmos_cast.h")
    lld_dir = str(Path(tools["ld.lld"]).parent)
    lld_link_dir = str(Path(tools["lld-link"]).parent)

    common = [
        "-ffreestanding",
        "-fno-stack-protector",
        "-fno-builtin",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-error=c23-extensions",
        "-g",
    ]
    trace = f"-DWASMOS_TRACE={cfg.flag('WASMOS_TRACE')}"

    # UEFI loader: PE/COFF via lld-link, compiled and linked in one step.
    efi = common + [
        "-target",
        "x86_64-unknown-uefi",
        "-fshort-wchar",
        "-mno-red-zone",
        f"-B{Path(clang).parent}",
        "-include",
        cast_h,
    ]
    if lld_link_dir != str(Path(clang).parent):
        efi.append(f"-B{lld_link_dir}")
    efi_ld = [
        "-g",
        "-target",
        "x86_64-unknown-uefi",
        f"-B{Path(clang).parent}",
        "-fuse-ld=lld-link",
        "-Wl,/subsystem:efi_application",
        "-Wl,/entry:efi_main",
    ]
    if lld_link_dir != str(Path(clang).parent):
        efi_ld.append(f"-B{lld_link_dir}")

    warp = cfg.WASMOS_WASM_RUNTIME_WARP
    runtime_inc = srcdir / ("libs/warp" if warp else "libs/wasm3")
    kernel = common + [
        "-target",
        cfg.get("KERNEL_TARGET_TRIPLE", "x86_64-elf"),
        "-mno-red-zone",
        "-mcmodel=kernel",
        "-m64",
        "-fno-jump-tables",
        f"-I{runtime_inc}",
        f"-I{srcdir / 'src/kernel'}",
        f"-I{srcdir / 'src/kernel/include'}",
        f"-I{srcdir / 'src/drivers/include'}",
        f"-I{srcdir / 'abi/generated/c'}",
        f"-B{lld_dir}",
        "-DWASMOS_ENABLE_PREEMPT_GUARD=1",
        "-DWASMOS_ENABLE_SAFEPOINT=1",
        f"-DWASMOS_WASM_RUNTIME={1 if warp else 0}",
        "-include",
        cast_h,
        "-fno-omit-frame-pointer",
        trace,
        f"-DWASMOS_RING3_SMOKE_DEFAULT={cfg.flag('WASMOS_RING3_SMOKE')}",
        f"-DWASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT={cfg.flag('WASMOS_RING3_THREAD_LIFECYCLE_SMOKE')}",
        f"-DWASMOS_PM_TEST_HOOKS={cfg.flag('WASMOS_PM_TEST_HOOKS')}",
        f"-DWASMOS_PM_LIST_IMPL={cfg.flag('WASMOS_PM_LIST_ARRAY_CHUNK')}",
        f"-DWASMOS_PM_LIST_ARRAY_CHUNK_CAP={cfg.get('WASMOS_PM_LIST_ARRAY_CHUNK_CAP', 16)}",
        f"-DWASMOS_IRQ_MODE={2 if cfg.WASMOS_IRQ_IOAPIC else 1 if cfg.WASMOS_IRQ_LAPIC else 0}",
        f"-DWASMOS_SMP={cfg.flag('WASMOS_SMP')}",
        f"-DWASMOS_SCHED_CALLER_CPU_BIAS={cfg.flag('WASMOS_SCHED_CALLER_CPU_BIAS')}",
    ]
    if cfg.WASMOS_SCHED_SMP_STRESS:
        kernel.append("-DWASMOS_SCHED_SMP_STRESS=1")
    kernel.append(
        f"-DWASMOS_WARP_RUNTIME_LOAD_AOT={cfg.flag('WASMOS_WARP_RUNTIME_LOAD_AOT')}"
    )
    if warp:
        kernel.append("-DWASMOS_WASM_RUNTIME_WARP")
    kernel.append("-Wno-unused-parameter")

    # WARP is C++14 with exceptions and RTTI on (it uses both internally), and
    # its POSIX-facing includes resolve to the kernel's compat headers first.
    warp_compat = f"-I{srcdir / 'src/kernel/warp/compat'}"
    warp_cxx = kernel + [
        "-std=c++14",
        "-frtti",
        "-fexceptions",
        "-Wno-deprecated",
        "-Wno-unused-variable",
        "-DACTIVE_STACK_OVERFLOW_CHECK=1",
        "-DLINEAR_MEMORY_BOUNDS_CHECKS=1",
        "-DVB_POSIX",
        "-DVB_WIN32_OR_POSIX",
        "-DNVALGRIND",
        warp_compat,
        "-isystem",
        f"{tools['clang-resource-dir']}/include",
        "-Wno-macro-redefined",
        "-Wno-c++20-extensions",
        "-Wno-deprecated-copy",
        "-Wno-#pragma-messages",
        # ENABLE_ADVANCED_APIS exposes getFunctionSignatureByName, which the
        # ring-3 IRET path uses to locate the JIT wrapper entry.
        "-DENABLE_ADVANCED_APIS=1",
    ]

    # Ring-3 native probes: position-dependent flat binaries the kernel embeds.
    probe = [
        "--target=x86_64-unknown-none-elf",
        "-Os",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-asynchronous-unwind-tables",
        "-fno-unwind-tables",
        "-fomit-frame-pointer",
        "-mno-red-zone",
        "-nostdlib",
        f"-I{libc_inc}",
    ]

    # wasm32 guest objects. Must match what scripts/sdk/wasmos-clang passes for
    # application sources, or an archive object and its caller disagree about,
    # for example, WASMOS_TRACE.
    wasm32 = [
        "--target=wasm32-unknown-unknown",
        "-Oz",
        "-nostdlib",
        "-ffreestanding",
        "-fno-builtin",
        "-D__wasmos__=1",
        trace,
        "-include",
        cast_h,
        f"-I{srcdir / 'src/libui/include'}",
        f"-I{libc_inc}",
        f"-I{srcdir / 'src/libsys/wasm/include'}",
        f"-I{srcdir / 'src/drivers/include'}",
    ]

    return {
        "efi": Toolchain(
            "efi", clang, clangxx, clang, tools["llvm-ar"], cflags=efi, ldflags=efi_ld
        ),
        "kernel": Toolchain(
            "kernel",
            clang,
            clangxx,
            tools["ld.lld"],
            tools["llvm-ar"],
            cflags=kernel,
            asflags=kernel,
            cxxflags=kernel
            + [
                "-fno-exceptions",
                "-fno-rtti",
                "-fno-threadsafe-statics",
                "-Wno-deprecated",
            ],
            ldflags=["-nostdlib"],
        ),
        "kernel-warp": Toolchain(
            "kernel-warp",
            clang,
            clangxx,
            tools["ld.lld"],
            tools["llvm-ar"],
            cflags=kernel + [warp_compat],
            cxxflags=warp_cxx,
        ),
        "probe": Toolchain(
            "probe",
            clang,
            clangxx,
            tools["ld.lld"],
            tools["llvm-ar"],
            cflags=probe,
            ldflags=["-nostdlib"],
        ),
        "wasm32": Toolchain(
            "wasm32", clang, clangxx, tools["wasm-ld"], tools["llvm-ar"], cflags=wasm32
        ),
        "host": Toolchain(
            "host",
            clang,
            clangxx,
            clang,
            tools["llvm-ar"],
            cflags=["-std=gnu11", "-O2"],
            cxxflags=["-O2"],
        ),
    }
