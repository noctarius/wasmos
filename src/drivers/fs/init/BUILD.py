"""fs_init: driver guest module."""

from wasmos_rules import guest


def build(b):
    guest(b, "fs_init", [b.src("fs_init.c")])
