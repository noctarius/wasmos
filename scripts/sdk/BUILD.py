"""Stage the relocatable WASMOS SDK: sysroot headers, wrappers, libexec, share/
and wasmos-sdk.conf (see scripts/sdk_stage.py and docs/toolchain.md)."""

SDK_VERSION = "0.1.0-spike"
# Layout constants the Zig driver enforces; cmake/WasmosZigApp.cmake documents them.
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
    b.exports["sdk"] = {"clang": clang, "packer": packer, "deps": deps}
    b.alias("wasmos-sdk", *deps)
