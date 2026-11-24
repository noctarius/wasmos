"""rtc_efi: the firmware clock driver (UEFI GetTime/SetTime), the efi backend
linked with the shared RTC core (src/drivers/rtc)."""

from wasmos_rules import guest, install


def build(b):
    core = b.root / "src/drivers/rtc"
    wap = guest(
        b,
        "rtc_efi",
        [b.src("rtc_efi.ts"), core / "rtc_core.ts", core / "rtc_time.ts"],
        lang="as",
    )
    install(b, "system/drivers/rtc_efi.wap", wap)
