"""virtio_rng: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "virtio_rng", [b.src("virtio_rng.c")])
    install(b, "system/drivers/virtio_rng.wap", wap)
