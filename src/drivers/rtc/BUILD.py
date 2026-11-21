"""rtc: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "rtc", [b.src("rtc.ts")], lang="as")
    install(b, "system/drivers/rtc.wap", wap)
