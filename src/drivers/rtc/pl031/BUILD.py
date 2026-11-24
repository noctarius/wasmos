"""pl031: the Arm PL031 clock driver (aarch64), the pl031 backend linked with
the shared RTC core (src/drivers/rtc/core)."""

from wasmos_rules import guest, install


def build(b):
    core = b.root / "src/drivers/rtc/core"
    wap = guest(
        b,
        "pl031",
        [b.src("pl031.ts"), core / "rtc_core.ts", core / "rtc_time.ts"],
        lang="as",
    )
    install(b, "system/drivers/pl031.wap", wap)
