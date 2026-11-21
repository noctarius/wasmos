"""volume_manager: service guest module."""

from wasmos_rules import guest


def build(b):
    guest(
        b,
        "volume_manager",
        [
            b.src("volume_manager.zig"),
            b.root / "src/libc/zig/driver.zig",
            b.src("recognise.zig"),
            b.src("recognise_fat.zig"),
            b.src("recognise_wfs.zig"),
            b.root / "src/drivers/partition_manager/partition_table.zig",
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
