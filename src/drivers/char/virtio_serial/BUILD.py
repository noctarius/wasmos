"""virtio_serial: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    # device-manager spawns it from the ESP (scripts/system/devmgr/rules/default.<arch>.rules).
    # logger_stage.c is the logger service's kernel-log ring follower; the
    # kernel-log port follows the same ring by the same rules.
    logger = b.root / "src/services/logger"
    wap = guest(
        b,
        "virtio_serial",
        [b.src("virtio_serial.c"), logger / "logger_stage.c"],
        flags=[f"-I{logger}"],
    )
    install(b, "system/drivers/virtio_serial.wap", wap)
