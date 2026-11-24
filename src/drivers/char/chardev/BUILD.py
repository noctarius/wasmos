"""chardev: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "chardev_server", [b.src("chardev_server.c")])
    install(b, "system/drivers/chardev_server.wap", wap)
