"""fs_tmpfs: driver guest module."""

from wasmos_rules import guest


def build(b):
    guest(
        b,
        "fs_tmpfs",
        [
            b.src("fs_tmpfs.zig"),
            b.src("fs_tmpfs_store.zig"),
            b.root / "src/libc/zig/driver.zig",
            b.root / "src/libsys/wasm/coroutine_wasm.c",
            b.root / "src/libsys/wasm/ipc_future_wasm.c",
            b.root / "src/libsys/wasm/service_runtime_wasm.c",
            b.root / "src/libsys/wasm/service_async_entry_wasm.c",
            b.root / "abi/generated/zig/wasmos_imports.zig",
            b.root / "abi/generated/zig/wasmos_opcodes.zig",
            b.root / "abi/generated/zig/wasmos_status.zig",
            b.root / "abi/generated/zig/wasmos_constants.zig",
        ],
        lang="zig",
    )
