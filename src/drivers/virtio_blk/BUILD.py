"""virtio_blk: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "virtio_blk",
        [
            b.src("virtio_blk.zig"),
            b.root / "src/libc/zig/driver.zig",
            b.root / "src/libc/zig/vring.zig",
            b.root / "src/libc/zig/vring_shim.c",
            b.root / "src/libc/zig/virtio_modern.zig",
            b.root / "src/libc/zig/virtio_modern_shim.c",
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
    install(b, "system/drivers/virtio_blk.wap", wap)
