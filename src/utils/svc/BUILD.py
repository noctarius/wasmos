"""svc: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "svc", [b.src("svc.c")])
    install(b, "system/utils/svc.wap", wap)
