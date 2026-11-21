"""mount: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "mount", [b.src("mount.c")])
    install(b, "system/utils/mount.wap", wap)
