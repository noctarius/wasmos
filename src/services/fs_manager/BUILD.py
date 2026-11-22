"""fs_manager: service guest module."""

from wasmos_rules import guest


def build(b):
    guest(
        b,
        "fs_manager",
        [
            b.src("fs_manager.c"),
            b.src("fs_manager_path.c"),
            b.src("fs_manager_backends.c"),
            b.src("fs_manager_clients.c"),
            b.src("fs_manager_pending.c"),
            b.src("fs_manager_units.c"),
        ],
    )
