"""serial: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "serial", [b.src("serial.ts"), b.src("serial_name.ts")], lang="as")
    install(b, "system/drivers/serial.wap", wap)
