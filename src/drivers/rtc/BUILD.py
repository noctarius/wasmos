"""rtc: the CMOS clock driver (x86_64), the cmos backend linked with the shared
RTC core (rtc_core.ts, rtc_time.ts)."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "rtc",
        [b.src("cmos.ts"), b.src("rtc_core.ts"), b.src("rtc_time.ts")],
        lang="as",
    )
    install(b, "system/drivers/rtc.wap", wap)
