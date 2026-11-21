"""host: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "host", [b.src("host.c")])
    install(b, "system/utils/host.wap", wap)
