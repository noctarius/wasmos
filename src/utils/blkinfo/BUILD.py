"""blkinfo: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "blkinfo", [b.src("blkinfo.c")])
    install(b, "system/utils/blkinfo.wap", wap)
