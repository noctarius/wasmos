"""virtio_input: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    # device-manager spawns one instance per virtio-input function
    # (scripts/system/devmgr/rules/default.aarch64.rules).
    wap = guest(b, "virtio_input", [b.src("virtio_input.c")])
    install(b, "system/drivers/virtio_input.wap", wap)
