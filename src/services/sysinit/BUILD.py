"""sysinit: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "sysinit", [b.src("init.c")], wasm="init.wasm")
    install(b, "system/services/sysinit.wap", wap)
    install(b, "apps/sysinit.wap", wap)
