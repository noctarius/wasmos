"""partprobe: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "partprobe", [b.src("partprobe.c")])
    install(b, "system/utils/partprobe.wap", wap)
