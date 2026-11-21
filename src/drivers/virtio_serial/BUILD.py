"""virtio_serial: driver guest module."""

from wasmos_rules import guest


def build(b):
    guest(b, "virtio_serial", [b.src("virtio_serial.c")])
