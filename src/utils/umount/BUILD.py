"""umount: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "umount", [b.src("umount.c")])
    install(b, "system/utils/umount.wap", wap)
