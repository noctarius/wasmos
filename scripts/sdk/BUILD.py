"""Stage the relocatable WASMOS SDK: sysroot headers, wrappers, libexec, share/
and wasmos-sdk.conf (see scripts/sdk_stage.py and docs/toolchain.md)."""

from wasmos_rules import install, lang_available

SDK_VERSION = "0.1.0-spike"
# Zig WASM layout, recorded in wasmos-sdk.conf for scripts/sdk/wasmos-zig.
#
# ZIG_STACK_SIZE is the shadow stack every Zig WASM app is built with
# (--stack). Zig's default is 1 MiB and is placed FIRST, so a module's globals
# land above 1 MiB and its declared linear memory must be at least 2 MiB. 8192
# gives the layout a C module has (stack_ptr = 0x2000, globals just above it),
# which lets a Zig app declare a single 64 KiB page like every C app. The reason
# is module size, not correctness.
#
# It is NOT a pointer-validity constraint. MEM_REGION_WASM_LINEAR is not a fixed
# 16-page user-VA mirror that a host-call pointer above 64 KiB falls outside of:
# mm_context_rebind_wasm_linear and mm_context_bind_wasm_linear_scattered
# (src/kernel/memory.c) repoint and resize that region to the guest's actual
# linear memory. Measured on 2026-08-21: a Zig module built with
# --stack 1048576 (data at 0x100000) executes console_write and reads its
# spawn-info buffer through xfer_buffer_read, with pointers above 1 MiB, under
# both runtimes; examples/rust/hello keeps its data at 0x100000 and passes its
# guest test.
#
# ZIG_VA_LIMIT is the budget scripts/wasm_stack_check.py enforces on stack
# pointer + data end. It checks that the small stack was applied -- a module
# that reverted to the 1 MiB default trips it -- and is not a bound the kernel
# imposes. See docs/TASKS.md before treating it as a safety property.
ZIG_STACK_SIZE = 8192
ZIG_VA_LIMIT = 32768
AS_SOURCES = [
    "src/libc/assemblyscript/wasmos.ts",
    "src/libc/assemblyscript/coroutine.ts",
    "src/libc/assemblyscript/eventloop.ts",
    "abi/generated/assemblyscript/wasmos_imports.ts",
    "abi/generated/assemblyscript/wasmos_status.ts",
]


def build(b):
    root, tools = b.root, b.tools
    sdk_dir = "wasmos-sdk"
    packer = b.exports["packer"]
    stamp = "sdk-obj/sysroot.stamp"
    as_sources = [root / s for s in AS_SOURCES]

    inputs = []
    for pattern in (
        "src/libc/include/*.h",
        "src/libc/include/sys/*.h",
        "src/libc/include/wasmos/*.h",
        "src/libsys/wasm/include/wasmos/*.h",
        "src/libui/include/wasmos/*.h",
        "src/drivers/include/*.h",
        "src/drivers/include/wasmos/*.h",
        "abi/generated/c/*.h",
        "src/libc/zig/*.zig",
        "src/libc/zig/compat/*.h",
    ):
        inputs += b.glob(pattern, root=root)
    inputs += b.glob("wasmos-*") + b.glob("Platform/*.cmake")
    inputs += [
        b.src("default-manifest.toml"),
        b.src("WASMOSToolchain.cmake"),
        root / "src/libc/assemblyscript/runtime.ts",
        root / "scripts/wasm_inspect.py",
        root / "scripts/wasm_stack_check.py",
        root / "tools/as_coroutine_transform.mjs",
        *as_sources,
    ]

    stage = root / "scripts/sdk_stage.py"
    argv = [
        tools["python"],
        stage,
        "--src",
        root,
        "--sdk",
        sdk_dir,
        "--packer",
        packer,
        "--stamp",
        stamp,
    ]
    for a in as_sources:
        argv += ["--as-source", a]
    for name, tool in (
        ("ar", "llvm-ar"),
        ("nm", "llvm-nm"),
        ("ranlib", "llvm-ranlib"),
        ("strip", "llvm-strip"),
        ("objdump", "llvm-objdump"),
        ("ld", "wasm-ld"),
    ):
        argv += ["--tool", f"{name}={tools[tool]}"]
    conf = {
        "VERSION": SDK_VERSION,
        "TARGET": "wasm32-unknown-wasmos",
        "TARGET_LLVM": "wasm32-unknown-unknown",
        "CLANG": tools["clang"],
        "CLANGXX": tools["clangxx"],
        "ZIG": tools["zig"],
        "ZIG_STACK_SIZE": ZIG_STACK_SIZE,
        "ZIG_VA_LIMIT": ZIG_VA_LIMIT,
        "ASC": tools["asc"],
        "RUSTC": tools["rustc"],
        "TINYGO": tools["tinygo"],
        "WASMOPT": tools["wasm-opt"],
    }
    argv += [x for k, v in conf.items() for x in ("--conf", f"{k}={v}")]

    clang = f"{sdk_dir}/bin/wasmos-clang"
    b.command(
        [stamp],
        argv,
        inputs=[stage],
        implicit=[packer, *inputs],
        implicit_outputs=[clang],
        description="SDK staging wasmos-sdk",
    )

    deps = [stamp, *b.exports["sdk.archives"]]
    b.exports["sdk"] = {
        "bin": f"{sdk_dir}/bin",
        "clang": clang,
        "packer": packer,
        "deps": deps,
    }
    b.alias("wasmos-sdk", *deps)

    # SDK smoke apps: each wrapper driven end to end from source to .wap with
    # the default manifest, the way an out-of-tree developer uses it.
    hellos = [
        ("c", "wasmos-clang", "examples/c/sdk_hello/hello.c", "sdkhello"),
        ("zig", "wasmos-zig", "examples/zig/sdk_hello/hello.zig", "sdkzig"),
        ("as", "wasmos-asc", "examples/assemblyscript/sdk_hello/hello.ts", "sdkas"),
        ("rust", "wasmos-rustc", "examples/rust/sdk_hello/hello.rs", "sdkrust"),
        ("go", "wasmos-tinygo", "examples/go/sdk_hello/hello.go", "sdkgo"),
    ]
    wasm32 = b.toolchain("wasm32").tc
    b.index(root / "examples/c/sdk_hello/hello.c", [wasm32.cc, *wasm32.cflags])
    for lang, driver, source, name in hellos:
        if not lang_available(b, lang):
            continue
        wap = f"{name}.wap"
        b.command(
            [wap],
            [f"{sdk_dir}/bin/{driver}", root / source, "-o", wap],
            inputs=[root / source],
            implicit=deps,
            description=f"SDK {wap}",
        )
        install(b, f"apps/{wap}", wap)
        b.alias("modules", wap)
