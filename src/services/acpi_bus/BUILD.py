"""acpi_bus: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "acpi_bus", [b.src("acpi_bus.c")])
    install(b, "system/services/acpi_bus.wap", wap)
