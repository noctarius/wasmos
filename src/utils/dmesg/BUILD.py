"""dmesg: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "dmesg", [b.src("dmesg.c")])
    install(b, "system/utils/dmesg.wap", wap)
