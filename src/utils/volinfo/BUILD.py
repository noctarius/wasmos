"""volinfo: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "volinfo", [b.src("volinfo.c")])
    install(b, "system/utils/volinfo.wap", wap)
