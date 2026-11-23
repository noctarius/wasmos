"""uart16550: UART driver guest module, the uart16550 backend linked with the shared
serial core (src/drivers/serial)."""

from wasmos_rules import guest, install


def build(b):
    core = b.root / "src/drivers/serial"
    wap = guest(
        b,
        "uart16550",
        [b.src("uart16550.ts"), core / "serial_core.ts", core / "serial_name.ts"],
        lang="as",
    )
    install(b, "system/drivers/uart16550.wap", wap)
