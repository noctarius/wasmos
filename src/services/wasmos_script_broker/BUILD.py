"""wasmos_script_broker: service guest module."""

from wasmos_rules import guest


def build(b):
    guest(b, "wamos_script_broker", [b.src("wamos_script_broker.c")])
