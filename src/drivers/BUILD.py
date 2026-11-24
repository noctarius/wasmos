"""Device drivers."""


def build(b):
    b.subdir(
        "chardev",
        "ata",
        "fs_fat",
        "fs_wfs",
        "fs_init",
        "fs_tmpfs",
        "uart16550",
        "pl011",
        "keyboard",
        "mouse",
        "rtc",
        "virtio_net",
        "virtio_serial",
        "virtio_rng",
        "virtio_blk",
        "partition_manager",
        "framebuffer",
        "framebuffer_pci",
        "framebuffer_ramfb",
    )
