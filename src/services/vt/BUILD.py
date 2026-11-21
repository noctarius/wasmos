"""vt: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "vt", [b.src("vt_main.c"), b.src("vt_outmux.c")])
    install(b, "system/services/vt.wap", wap)
    install(b, "apps/vt.wap", wap)
