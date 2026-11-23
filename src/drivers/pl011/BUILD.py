"""pl011: UART driver guest module, the pl011 backend linked with the shared
serial core (src/drivers/serial)."""

from wasmos_rules import guest, install


def build(b):
    core = b.root / "src/drivers/serial"
    wap = guest(
        b,
        "pl011",
        [
            b.src("pl011.ts"),
            core / "serial_core.ts",
            core / "serial_name.ts",
            core / "uart_window.ts",
            core / "uart.ts",
        ],
        lang="as",
    )
    install(b, "system/drivers/pl011.wap", wap)
