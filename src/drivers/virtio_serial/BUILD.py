"""virtio_serial: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    # device-manager spawns it from the ESP (scripts/system/devmgr/rules/default.rules).
    wap = guest(b, "virtio_serial", [b.src("virtio_serial.c")])
    install(b, "system/drivers/virtio_serial.wap", wap)
