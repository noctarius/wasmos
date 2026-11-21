"""explorer: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "explorer", [b.src("explorer.c")])
    install(b, "system/utils/explorer.wap", wap)
