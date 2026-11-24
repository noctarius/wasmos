"""virtio_gpu: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    # device-manager spawns it for the virtio-gpu function
    # (scripts/system/devmgr/rules/default.<arch>.rules).
    wap = guest(
        b,
        "virtio_gpu",
        [b.src("virtio_gpu.c"), b.src("virtio_gpu_damage.c")],
    )
    install(b, "system/drivers/virtio_gpu.wap", wap)
