"""device_manager: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "device_manager",
        [
            b.src("device_manager.c"),
            b.src("device_manager_rules.c"),
            b.src("device_manager_unit.c"),
        ],
    )
    install(b, "system/services/device_manager.wap", wap)
