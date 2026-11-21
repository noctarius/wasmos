"""mouse: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "mouse", [b.src("mouse.ts")], lang="as")
    install(b, "system/drivers/mouse.wap", wap)
