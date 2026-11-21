"""pci_bus: service guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "pci_bus", [b.src("pci_bus.c")])
    install(b, "system/services/pci_bus.wap", wap)
