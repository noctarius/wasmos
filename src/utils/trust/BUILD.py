"""trust: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "trust",
        [b.src("trust.c")],
        flags=b.exports["monocypher"]["include"],
        libs=["-lmonocypher"],
    )
    install(b, "system/utils/trust.wap", wap)
