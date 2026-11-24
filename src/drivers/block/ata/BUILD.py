"""ata: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "ata", [b.src("ata.c")])
    install(b, "system/drivers/ata.wap", wap)
