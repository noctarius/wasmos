"""cli: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "cli",
        [
            b.src("cli.c"),
            b.src("cli_ls_order.c"),
            b.root / "src/libsys/wasm/ipc_future_wasm.c",
        ],
    )
    install(b, "system/services/cli.wap", wap)
    install(b, "apps/cli.wap", wap)
