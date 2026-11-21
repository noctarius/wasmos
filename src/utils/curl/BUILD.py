"""curl: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "curl", [b.src("curl.c")])
    install(b, "system/utils/curl.wap", wap)
