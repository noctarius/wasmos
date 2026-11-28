"""Kernel: kernel.elf, linked twice so the final image embeds its own symbol
table (scripts/gen_kernel_kallsyms.py reads the first link's symbols)."""

from toolchains import arch

# Per-architecture sources. `head` links first, so the image starts with the
# entry point; `sources` link after the neutral core. An architecture whose
# `core` is False is a bring-up stub: it links its own sources alone.
ARCH = {
    "x86_64": {
        "head": [
            "arch/x86_64/entry.S",
            "arch/x86_64/context_switch.S",
            "arch/x86_64/cpu_isr.S",
            "arch/x86_64/smp_trampoline.S",
        ],
        "sources": [
            "arch/x86_64/cpu_x86_64.c",
            "arch/x86_64/paging.c",
            "arch/x86_64/irq_x86_64.c",
            "arch/x86_64/msi_x86_64.c",
            "arch/x86_64/lapic.c",
            "arch/x86_64/ioapic.c",
            "arch/x86_64/smp.c",
            "arch/x86_64/platform_x86_64.c",
            "arch/x86_64/pci_cfg_x86_64.c",
            "arch/x86_64/timer_x86_64.c",
        ],
        # Compiled with the WARP glue, only when WARP is the runtime. The SJLJ
        # unwinder is dormant (WARP compiles with Dwarf -fexceptions) and its
        # _Unwind_FunctionContext layout is the x86_64 one.
        "warp": ["arch/x86_64/warp_ring3_x86_64.c", "warp/sjlj_unwind.cpp"],
        # WARP's code generator for this architecture (libs/warp/src/...).
        "warp_backend": [
            f"src/core/compiler/backend/x86_64/x86_64_{s}.cpp"
            for s in (
                "assembler",
                "backend",
                "call_dispatch",
                "cc",
                "instruction",
                "relpatchobj",
            )
        ],
        "ldscript": "arch/x86_64/linker.ld",
        "core": True,
    },
    "aarch64": {
        "head": [
            "arch/aarch64/entry.S",
            "arch/aarch64/vectors.S",
            "arch/aarch64/context_switch.S",
        ],
        "sources": [
            "arch/aarch64/cpu_aarch64.c",
            "arch/aarch64/paging.c",
            "arch/aarch64/gic_aarch64.c",
            "arch/aarch64/irq_aarch64.c",
            "arch/aarch64/timer_aarch64.c",
            "arch/aarch64/acpi_aarch64.c",
            "arch/aarch64/pci_cfg_aarch64.c",
            "arch/aarch64/its_aarch64.c",
            "arch/aarch64/msi_aarch64.c",
            "arch/aarch64/platform_aarch64.c",
            "arch/aarch64/smp_aarch64.c",
        ],
        "warp": [
            "arch/aarch64/warp_ring3_aarch64.c",
            "arch/aarch64/warp_jmp_aarch64.S",
        ],
        "warp_backend": [
            f"src/core/compiler/backend/aarch64/aarch64_{s}.cpp"
            for s in (
                "assembler",
                "aux",
                "backend",
                "call_dispatch",
                "cc",
                "instruction",
                "relpatchobj",
            )
        ],
        "ldscript": "arch/aarch64/linker.ld",
        "core": True,
    },
}

# Architecture-neutral kernel sources, in link order.
CORE = [
    "kernel.c",
    "kpanic.c",
    "kallsyms.c",
    "kernel_boot_runtime.c",
    "kernel_init_runtime.c",
    "kernel_selftest_runtime.c",
    "kernel_sched_selftest_runtime.c",
    "kernel_ring3_fault_runtime.c",
    "kernel_ring3_probe_runtime.c",
    "kernel_ring3_smoke_runtime.c",
    "kernel_ring3_suite_runtime.c",
    "kernel_threading_selftest_runtime.c",
    "kernel_sched_smp_stress_runtime.c",
    "memory.c",
    "capability.c",
    "pci_config.c",
    "policy.c",
    "slab.c",
    "syscall.c",
    "libc.c",
    "klog.c",
    "kmem.c",
    "list.c",
    "list_linked.c",
    "list_array_chunk.c",
    "hashmap.c",
    "subsystem_registry.c",
    "service_class_registry.c",
    "service_name_watch.c",
    "wasmos_exec_format.c",
    "linmem_slots.c",
    "physmem.c",
    "runtime_context.c",
    "ipc.c",
    "idtable.c",
    "block_buffer.c",
    "hostcall_dma.c",
    "hostcall_value.c",
    "phys_map.c",
    "kenv.c",
    "kmsg.c",
    "hostcall_buffer.c",
    "process.c",
    "thread.c",
    "sched_thread.c",
    "sched_event.c",
    "poll.c",
    "futex.c",
    "spinlock.c",
    "sync_mutex.c",
    "sync_semaphore.c",
    "user_mutex.c",
    "memory_service.c",
    "irq_sharing.c",
    "msi_vectors.c",
    "mmio.c",
    "device_window.c",
    "io_region.c",
    "timer.c",
    "process_manager.cpp",
    "xfer_buffer/xfer_buffer.c",
    "warp_driver_ring3_call_policy.c",
    "process_manager_services.c",
    "process_manager_spawn.c",
    "process_manager_caps.c",
    "process_manager_shutdown.c",
    "shutdown_order.c",
    "wasmos_app.c",
    "wasmos_app_meta.c",
    "native_driver.c",
    "serial.c",
    "framebuffer.c",
    "efi_rtc.c",
]

WARP_GLUE = [
    "warp_driver.cpp",
    "warp/shim.cpp",
    "warp/link.cpp",
    "warp/link_dma.cpp",
    "warp/link_ipc.cpp",
    "warp/link_rtc.cpp",
    "warp/cxx_abi.cpp",
    "warp/exception_checkpoint.c",
    "warp/mem_utils_kernel.cpp",
    "warp/os_api_checker_kernel.cpp",
    "warp/iostream_stubs.cpp",
    "warp/ring3_trampolines.c",
    "warp/linker_stubs.cpp",
]

# Vendored WARP sources (libs/warp is a git subtree; never edited here), without
# the code generator, which is per architecture (ARCH[...]["warp_backend"]).
WARP_LIB = [
    "src/WasmModule/WasmModule.cpp",
    "src/core/runtime/Runtime.cpp",
    "src/core/runtime/ActiveMemoryManager.cpp",
    "src/core/runtime/MemoryHelper.cpp",
    "src/core/common/BinaryModule.cpp",
    "src/core/common/VbExceptions.cpp",
    "src/core/common/ExtendableMemory.cpp",
    "src/utils/VirtualMemoryAllocator.cpp",
    "src/core/compiler/Compiler.cpp",
    "src/core/compiler/backend/BackendBase.cpp",
    "src/core/compiler/common/Common.cpp",
    "src/core/compiler/common/CommonRegsPos.cpp",
    "src/core/compiler/common/MemWriter.cpp",
    "src/core/compiler/common/ModuleInfo.cpp",
    "src/core/compiler/common/Stack.cpp",
    "src/core/compiler/frontend/BytecodeReader.cpp",
    "src/core/compiler/frontend/Frontend.cpp",
    "src/core/compiler/frontend/ValidationStack.cpp",
]

# Kernel-side wasm3 glue, compiled with the plain kernel flags.
WASM3_GLUE_KERNEL = ["wasm_driver.c"]
WASM3_GLUE = ["wasm3/link.c", "wasm3/link_dma.c", "wasm3/link_ipc.c"]
WASM3_SHIM = ["wasm3/shim.c"]
WASM3_LIB = [
    "m3_code.c",
    "m3_bind.c",
    "m3_compile.c",
    "m3_core.c",
    "m3_env.c",
    "m3_exec.c",
    "m3_function.c",
    "m3_info.c",
    "m3_module.c",
    "m3_parse.c",
]

RING3_PROBES = ["ring3_native_probe", "ring3_thread_lifecycle_probe"]


def ring3_probe_blob(b, name):
    """Compile a ring-3 native probe to a flat binary and wrap it as an ELF
    object. llvm-objcopy derives the _binary_<file>_start/_end symbols the
    kernel references from the input file NAME, so it runs in the build
    directory on a bare filename."""
    target = arch(b.cfg)
    probe = b.toolchain("probe")
    (obj,) = probe.compile([b.src(f"{name}.c")])
    ldscript = b.src(f"arch/{target.name}/ring3_native.ld")
    elf = probe.link(
        f"{name}.elf", [obj], flags=["-T", str(ldscript)], implicit=[ldscript]
    )
    objcopy = b.tools["llvm-objcopy"]
    b.command(
        [f"{name}.bin"],
        [objcopy, "--output-target=binary", elf, f"{name}.bin"],
        inputs=[elf],
        description=f"OBJCOPY {name}.bin",
    )
    return b.command(
        [f"{name}_blob.o"],
        [
            objcopy,
            "--input-target=binary",
            f"--output-target={target.objcopy_target}",
            f"--binary-architecture={target.objcopy_binary_arch}",
            f"{name}.bin",
            f"{name}_blob.o",
        ],
        inputs=[f"{name}.bin"],
        description=f"BLOB {name}_blob.o",
    )[0]


def build(b):
    cfg = b.cfg
    kernel = b.toolchain("kernel")
    warp = cfg.WASMOS_WASM_RUNTIME_WARP
    target = ARCH[arch(cfg).name]
    ldscript = b.src(target["ldscript"])
    ldflags = ["-T", str(ldscript)]

    if not target["core"]:
        objs = kernel.compile(b.srcs(*target["head"], *target["sources"]))
        elf = kernel.link("kernel.elf", objs, flags=ldflags, implicit=[ldscript])
        b.alias("kernel", elf, default=True)
        b.exports["kernel.elf"] = elf
        return

    core = [*target["head"], *CORE, *target["sources"]]
    if warp:
        objs = kernel.compile(b.srcs(*core), flags=b.exports["monocypher"]["include"])
        kwarp = b.toolchain("kernel-warp")
        # Assembly takes the kernel profile's flags; kernel-warp carries C/C++ only.
        warp_arch = target.get("warp", [])
        objs += kernel.compile(b.srcs(*[s for s in warp_arch if s.endswith(".S")]))
        objs += kwarp.compile(
            b.srcs(*WARP_GLUE, *[s for s in warp_arch if not s.endswith(".S")])
        )
        objs += kwarp.compile(
            [b.root / "libs/warp" / s for s in WARP_LIB + target["warp_backend"]]
        )
    else:
        wasm3_inc = [f"-I{b.root / 'libs/wasm3/source'}"]
        wasm3_lib = ["-Wno-unused-variable"]
        objs = kernel.compile(
            b.srcs(*core), flags=wasm3_inc + b.exports["monocypher"]["include"]
        )
        objs += kernel.compile(b.srcs(*WASM3_GLUE_KERNEL), flags=wasm3_inc)
        objs += kernel.compile(b.srcs(*WASM3_GLUE), flags=wasm3_inc + wasm3_lib)
        objs += kernel.compile(b.srcs(*WASM3_SHIM), flags=wasm3_inc)
        objs += kernel.compile(
            [b.root / "libs/wasm3/source" / s for s in WASM3_LIB],
            flags=wasm3_inc + wasm3_lib,
        )

    # Ed25519 (libs/monocypher) and the build's public key: the kernel checks
    # package signatures with them (wasmos_app_signature_valid).
    mono = b.exports["monocypher"]
    objs += kernel.compile(mono["sources"], flags=mono["include"])
    objs += kernel.compile(
        [b.exports["signing.pubkey_c"]],
        obj_names={b.exports["signing.pubkey_c"]: "wasmos_signing_pubkey.o"},
    )

    # Index the other runtime's first-party glue, with the flags a tree of that
    # runtime compiles it with, so one lint pass covers both runtimes.
    other = {"WASMOS_WASM_RUNTIME_WARP": not warp, "WASMOS_WASM_RUNTIME_WASM3": warp}
    if warp:
        tc = b.toolchain("kernel", overrides=other).tc
        wasm3_inc = [f"-I{b.root / 'libs/wasm3/source'}"]
        for src in b.srcs(*WASM3_GLUE_KERNEL, *WASM3_GLUE, *WASM3_SHIM):
            b.index(src, [tc.cc, *tc.cflags, *wasm3_inc])
    else:
        tc = b.toolchain("kernel-warp", overrides=other).tc
        warp_arch = [s for s in target.get("warp", []) if not s.endswith(".S")]
        for src in b.srcs(*WARP_GLUE, *warp_arch):
            cxx = src.suffix == ".cpp"
            b.index(
                src, [tc.cxx if cxx else tc.cc, *(tc.cxxflags if cxx else tc.cflags)]
            )

    # Index the other architectures' C sources with their own kernel flags, so
    # the same lint pass covers every arch directory. They take the include path
    # this tree's core sources take, because they include the same headers.
    core_inc = [] if warp else [f"-I{b.root / 'libs/wasm3/source'}"]
    for name, spec in ARCH.items():
        if name == arch(cfg).name:
            continue
        sets = {
            "WASMOS_ARCH_AARCH64": name == "aarch64",
            "WASMOS_ARCH_X86_64": name == "x86_64",
        }
        tc = b.toolchain("kernel", overrides=sets).tc
        for src in b.srcs(*spec["sources"]):
            b.index(src, [tc.cc, *tc.cflags, *core_inc])
        # The architecture's WARP glue, as that architecture's WARP tree
        # compiles it.
        warp_sets = {
            **sets,
            "WASMOS_WASM_RUNTIME_WARP": True,
            "WASMOS_WASM_RUNTIME_WASM3": False,
        }
        tc = b.toolchain("kernel-warp", overrides=warp_sets).tc
        for src in b.srcs(*spec.get("warp", [])):
            cxx = src.suffix == ".cpp"
            b.index(
                src, [tc.cxx if cxx else tc.cc, *(tc.cxxflags if cxx else tc.cflags)]
            )

    # Embedded blobs: guest modules registered by other manifests first, then
    # the native ring-3 probes.
    blobs = list(b.exports.get("kernel.blobs", []))
    blobs += [ring3_probe_blob(b, p) for p in RING3_PROBES]

    base = kernel.link(
        "kernel.base.elf", objs + blobs, flags=ldflags, implicit=[ldscript]
    )

    gen = b.root / "scripts/gen_kernel_kallsyms.py"
    b.command(
        ["kernel_kallsyms_data.c"],
        [
            b.tools["python"],
            gen,
            "--nm",
            b.tools["llvm-nm"],
            "--input",
            base,
            "--output",
            "kernel_kallsyms_data.c",
        ],
        inputs=[base],
        implicit=[gen],
        description="KALLSYMS kernel_kallsyms_data.c",
    )
    kallsyms = kernel.compile(
        [b.out / "kernel_kallsyms_data.c"],
        obj_names={b.out / "kernel_kallsyms_data.c": "kernel_kallsyms_data.o"},
    )

    elf = kernel.link(
        "kernel.elf", objs + blobs + kallsyms, flags=ldflags, implicit=[ldscript]
    )
    b.alias("kernel", elf, default=True)
    b.exports["kernel.elf"] = elf
