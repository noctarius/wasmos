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
import sys
from dataclasses import dataclass, field
from pathlib import Path

LLVM_HINTS = [
    "/opt/homebrew/opt/llvm/bin",
    "/usr/local/opt/llvm/bin",
    "/opt/homebrew/bin",
    "/usr/local/bin",
]
LLD_HINTS = ["/opt/homebrew/opt/lld/bin", "/usr/local/opt/lld/bin"]


OVMF_CODE_CANDIDATES = [
    "/usr/share/OVMF/OVMF_CODE.fd",
    "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/edk2/ovmf/OVMF_CODE.fd",
    "/usr/share/edk2/x64/OVMF_CODE.4m.fd",
    "/opt/homebrew/share/OVMF/OVMF_CODE.fd",
    "/usr/local/share/OVMF/OVMF_CODE.fd",
    "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
    "/opt/homebrew/share/qemu/edk2-i386-code.fd",
    "/usr/local/share/qemu/edk2-x86_64-code.fd",
    "/usr/local/share/qemu/edk2-i386-code.fd",
    "/opt/homebrew/share/qemu/edk2-x86_64-secure-code.fd",
    "/usr/local/share/qemu/edk2-x86_64-secure-code.fd",
    "/opt/homebrew/share/edk2-ovmf/OVMF_CODE.fd",
    "/usr/local/share/edk2-ovmf/OVMF_CODE.fd",
    "/opt/homebrew/share/edk2/OVMF/OVMF_CODE.fd",
    "/usr/local/share/edk2/OVMF/OVMF_CODE.fd",
]
OVMF_VARS_CANDIDATES = [
    "/usr/share/OVMF/OVMF_VARS.fd",
    "/opt/homebrew/share/OVMF/OVMF_VARS.fd",
    "/usr/local/share/OVMF/OVMF_VARS.fd",
    "/opt/homebrew/share/qemu/edk2-x86_64-vars.fd",
    "/opt/homebrew/share/qemu/edk2-i386-vars.fd",
    "/usr/local/share/qemu/edk2-x86_64-vars.fd",
    "/usr/local/share/qemu/edk2-i386-vars.fd",
    "/opt/homebrew/share/edk2-ovmf/OVMF_VARS.fd",
    "/usr/local/share/edk2-ovmf/OVMF_VARS.fd",
    "/opt/homebrew/share/edk2/OVMF/OVMF_VARS.fd",
    "/usr/local/share/edk2/OVMF/OVMF_VARS.fd",
]


# edk2 for QEMU virt (aarch64). Both images must be the 64 MiB pflash size the
# machine expects; Homebrew and Debian (qemu-efi-aarch64) ship them padded.
AAVMF_CODE_CANDIDATES = [
    "/opt/homebrew/share/qemu/edk2-aarch64-code.fd",
    "/usr/local/share/qemu/edk2-aarch64-code.fd",
    "/usr/share/AAVMF/AAVMF_CODE.fd",
    "/usr/share/qemu/edk2-aarch64-code.fd",
    "/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw",
    "/usr/share/edk2/aarch64/QEMU_CODE.fd",
]


@dataclass(frozen=True)
class Arch:
    """Everything that differs per target architecture outside the sources:
    compiler targets and ABI flags, the ELF format kernel blobs are wrapped in,
    the UEFI boot file name, and the QEMU machine a tree boots on. buildgen,
    the manifests and the QEMU harness (scripts/qemu_test_framework.py) all read
    this table, so an architecture is described exactly once."""

    name: str
    # Kernel and native ring-3 code.
    triple: str
    efi_triple: str
    efi_boot_name: str
    # Code-model and ABI flags every kernel object carries.
    kernel_flags: tuple
    # ABI flags every native ring-3 object carries.
    native_flags: tuple
    zig_target: str
    # llvm-objcopy --output-target / --binary-architecture for a raw blob.
    objcopy_target: str
    objcopy_binary_arch: str
    # src/libsys/native/<file>: the native coroutine context switch.
    coroutine_asm: str
    # buildgen-tools.json keys of the QEMU binary and its firmware images.
    qemu_tool: str
    firmware_code_tool: str
    firmware_vars_tool: str
    # Machine and CPU arguments; the harness adds -accel per host.
    qemu_machine: tuple
    # Whether the machine has an IDE controller (the ata driver's WFS unit).
    qemu_ide: bool
    # Whether a virtio-net device keeps QEMU's option ROM. The ROM QEMU ships
    # (efi-virtio.rom) is an x86 iPXE image: aarch64 firmware cannot run it,
    # and a distribution QEMU without it refuses to start the device.
    qemu_nic_option_rom: bool


ARCHES = {
    "x86_64": Arch(
        name="x86_64",
        triple="x86_64-elf",
        efi_triple="x86_64-unknown-uefi",
        efi_boot_name="BOOTX64.EFI",
        # The kernel is linked in the top 2 GiB (-mcmodel=kernel) and takes
        # interrupts on the current stack, so it must not use a red zone.
        kernel_flags=("-mno-red-zone", "-mcmodel=kernel", "-m64"),
        native_flags=("-mno-red-zone", "-mcmodel=small"),
        zig_target="x86_64-freestanding",
        objcopy_target="elf64-x86-64",
        objcopy_binary_arch="i386:x86-64",
        coroutine_asm="coroutine_native_x86_64.S",
        qemu_tool="qemu",
        firmware_code_tool="ovmf-code",
        firmware_vars_tool="ovmf-vars",
        qemu_machine=(),
        qemu_ide=True,
        qemu_nic_option_rom=True,
    ),
    "aarch64": Arch(
        name="aarch64",
        triple="aarch64-elf",
        # clang has no aarch64 UEFI triple. The Windows one emits the same
        # PE/COFF for lld-link, and Windows on Arm's calling convention is
        # AAPCS64, which is what UEFI firmware on aarch64 uses.
        efi_triple="aarch64-unknown-windows",
        efi_boot_name="BOOTAA64.EFI",
        # The small model reaches symbols through ADRP (+/-4 GiB PC-relative),
        # which holds for a kernel linked anywhere in the TTBR1 half. AAPCS64
        # has no red zone. Kernel code may use FP/SIMD registers, as wasm3's
        # float opcodes do, so entry must enable them (CPACR_EL1.FPEN).
        kernel_flags=("-mcmodel=small",),
        native_flags=("-mcmodel=small",),
        zig_target="aarch64-freestanding",
        objcopy_target="elf64-littleaarch64",
        objcopy_binary_arch="aarch64",
        coroutine_asm="coroutine_native_aarch64.S",
        qemu_tool="qemu-aarch64",
        firmware_code_tool="aavmf-code",
        firmware_vars_tool="aavmf-vars",
        qemu_machine=("-M", "virt"),
        qemu_ide=False,
        qemu_nic_option_rom=False,
    ),
}


def arch(cfg) -> Arch:
    """The target architecture a configuration selects."""
    return ARCHES["aarch64" if cfg.WASMOS_ARCH_AARCH64 else "x86_64"]


def _first_existing(candidates: list[str]) -> str:
    return next((c for c in candidates if Path(c).is_file()), "")


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
    tools["asc"] = (
        overrides.get("asc")
        or _find("asc", [str(srcdir / "node_modules/.bin"), bindir], required=False)
        or ""
    )
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
    # Quality-gate tools (scripts/quality.sh); each language's checks need only
    # its own. CLANG_FORMAT in the environment pins the formatter version.
    tools["clang-format"] = (
        overrides.get("clang-format")
        or os.environ.get("CLANG_FORMAT")
        or _find("clang-format", hints, required=False)
        or ""
    )
    # Directory providing clang-tidy/*.h when the LLVM install lacks them (CI
    # fetches clang-tools-extra); the plugin build adds it to its includes.
    tools["clang-tidy-include"] = overrides.get("clang-tidy-include") or os.environ.get(
        "WASMOS_CLANG_TIDY_INCLUDE_DIR", ""
    )
    for t in ("clang-tidy", "llvm-config"):
        tools[t] = overrides.get(t) or _find(t, hints, required=False) or ""
    # black/ruff usually live beside the Python that runs configure (a venv).
    pybin = str(Path(sys.executable).parent)
    for t in ("gofmt", "go", "rustfmt", "black", "ruff"):
        tools[t] = (
            overrides.get(t)
            or _find(t, [pybin, f"{home}/.cargo/bin"], required=False)
            or ""
        )
    tools["host-os"] = platform.system()
    tools["qemu"] = (
        overrides.get("qemu") or _find("qemu-system-x86_64", [], required=False) or ""
    )
    tools["qemu-aarch64"] = (
        overrides.get("qemu-aarch64")
        or _find("qemu-system-aarch64", [], required=False)
        or ""
    )
    tools["ovmf-code"] = overrides.get("ovmf-code") or _first_existing(
        OVMF_CODE_CANDIDATES
    )
    tools["ovmf-vars"] = overrides.get("ovmf-vars") or _first_existing(
        OVMF_VARS_CANDIDATES
    )
    # No aarch64 vars store is discovered: without one edk2 keeps its variables
    # in RAM, and a shared, writable store would carry state between boots.
    tools["aavmf-code"] = overrides.get("aavmf-code") or _first_existing(
        AAVMF_CODE_CANDIDATES
    )
    tools["aavmf-vars"] = overrides.get("aavmf-vars") or ""
    # An i386 vars store beside x86_64 firmware hangs the boot; drop it.
    if "i386" in tools["ovmf-vars"] and "x86_64" in tools["ovmf-code"]:
        tools["ovmf-vars"] = ""
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
    target = arch(cfg)
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
        target.efi_triple,
        "-fshort-wchar",
        *(["-mno-red-zone"] if target.name == "x86_64" else []),
        # The Windows triple (aarch64) emits __chkstk probes for large frames;
        # firmware provides no such routine.
        *(["-mno-stack-arg-probe"] if target.name != "x86_64" else []),
        f"-B{Path(clang).parent}",
        "-include",
        cast_h,
    ]
    if lld_link_dir != str(Path(clang).parent):
        efi.append(f"-B{lld_link_dir}")
    efi_ld = [
        "-g",
        "-target",
        target.efi_triple,
        f"-B{Path(clang).parent}",
        "-fuse-ld=lld-link",
        "-Wl,/subsystem:efi_application",
        "-Wl,/entry:efi_main",
        # The Windows triple (aarch64) names the MSVC runtime libraries as
        # defaults; a UEFI application links none.
        *(["-nostdlib", "-Wl,/nodefaultlib"] if target.name != "x86_64" else []),
    ]
    if lld_link_dir != str(Path(clang).parent):
        efi_ld.append(f"-B{lld_link_dir}")

    warp = cfg.WASMOS_WASM_RUNTIME_WARP
    runtime_inc = srcdir / ("libs/warp" if warp else "libs/wasm3")
    kernel = common + [
        "-target",
        target.triple,
        *target.kernel_flags,
        # Selects include/arch/<arch>/ in the arch selection headers. Keyed on
        # this rather than on __aarch64__ so a host unit-test build on an arm64
        # host keeps the headers it tests.
        f"-DWASMOS_ARCH_{target.name.upper()}=1",
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
        f"--target={target.name}-unknown-none-elf",
        "-Os",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-asynchronous-unwind-tables",
        "-fno-unwind-tables",
        "-fomit-frame-pointer",
        *(["-mno-red-zone"] if target.name == "x86_64" else []),
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
                # Kernel C++ uses no C++ standard library; without this the
                # aarch64 bare-metal driver resolves <stddef.h> to libc++'s
                # wrapper, which needs a hosted configuration.
                "-nostdinc++",
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
        # Native ring-3 modules (framebuffer drivers, Zig/C services). Their
        # flag sets differ per module, so the profile carries only the target.
        "native": Toolchain(
            "native",
            clang,
            clangxx,
            tools["ld.lld"],
            tools["llvm-ar"],
            cflags=["-target", target.triple, *target.native_flags],
            asflags=["-target", target.triple],
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
