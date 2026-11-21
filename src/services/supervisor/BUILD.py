"""supervisor: service guest module."""

from wasmos_rules import guest


def build(b):
    guest(b, "supervisor", [b.src("supervisor.c"), b.src("supervisor_unit.c")])
