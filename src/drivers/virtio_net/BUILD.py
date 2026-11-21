"""virtio_net: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "virtio_net", [b.src("virtio_net.c")])
    install(b, "system/drivers/virtio_net.wap", wap)
