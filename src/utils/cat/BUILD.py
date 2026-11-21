"""cat: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "cat", [b.src("cat.zig")], lang="zig")
    install(b, "system/utils/cat.wap", wap)
