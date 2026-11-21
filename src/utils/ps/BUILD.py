"""ps: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "ps", [b.src("ps.zig")], lang="zig")
    install(b, "system/utils/ps.wap", wap)
