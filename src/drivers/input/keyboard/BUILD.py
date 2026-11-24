"""keyboard: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "keyboard", [b.src("keyboard.ts")], lang="as")
    install(b, "system/drivers/keyboard.wap", wap)
