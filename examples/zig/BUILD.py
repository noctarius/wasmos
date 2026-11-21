"""Zig example guest modules."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "calculator",
        [
            b.src("calculator/calculator.zig"),
            b.root / "src/libsys/wasm/coroutine_wasm.c",
            b.root / "src/libsys/wasm/ipc_future_wasm.c",
            b.root / "src/libui/zig/libui.zig",
            b.root / "src/libui/zig/libui_shim.c",
        ],
        manifest=b.src("calculator/linker.metadata"),
        lang="zig",
    )
    install(b, "apps/calculator.wap", wap)
    wap = guest(
        b,
        "hello_zig",
        [
            b.src("hello/hello_zig.zig"),
            b.root / "src/libsys/wasm/coroutine_wasm.c",
            b.root / "src/libsys/wasm/ipc_future_wasm.c",
            b.root / "src/libsys/wasm/service_runtime_wasm.c",
        ],
        lang="zig",
    )
    install(b, "apps/hello_zig.wap", wap)
