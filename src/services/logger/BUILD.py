"""logger: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "logger",
        [
            b.root / "src/libsys/wasm/service_async_entry_wasm.c",
            b.src("logger.c"),
            b.src("logger_stage.c"),
        ],
    )
    install(b, "system/services/logger.wap", wap)
