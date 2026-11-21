"""ip: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "ip", [b.src("ip.c")])
    install(b, "system/utils/ip.wap", wap)
