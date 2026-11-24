"""Device drivers, grouped by the device class they provide (as Linux groups
drivers/): a virtio device driver sits with its class, not under a virtio
directory, since virtio is a transport (libsys virtio_modern.h / vring.h).
Installed package names (system/drivers/*.wap) are independent of this layout."""


def build(b):
    b.subdir(
        "char/chardev",
        "block/ata",
        "fs/fat",
        "fs/wfs",
        "fs/init",
        "fs/tmpfs",
        "tty/uart16550",
        "tty/pl011",
        "input/keyboard",
        "input/mouse",
        "rtc/cmos",
        "rtc/pl031",
        "rtc/efi",
        "net/virtio_net",
        "char/virtio_serial",
        "char/virtio_rng",
        "input/virtio_input",
        "gpu/virtio_gpu",
        "block/virtio_blk",
        "block/partition",
        "gpu/framebuffer",
        "gpu/bochs",
        "gpu/ramfb",
    )
