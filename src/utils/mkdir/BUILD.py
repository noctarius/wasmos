"""mkdir: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "mkdir", [b.src("mkdir.c")])
    install(b, "system/utils/mkdir.wap", wap)
