"""System services."""


def build(b):
    b.subdir(
        "sysinit",
        "cli",
        "device_manager",
        "pci_bus",
        "acpi_bus",
        "fs_manager",
        "vt",
        "logger",
        "supervisor",
        "gfx_compositor",
        "font_service",
        "net_stack",
        "wasmos_script",
        "wasmos_script_broker",
        "volume_manager",
    )
