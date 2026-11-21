"""Rust example guest modules."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "hello_rust", [b.src("hello/hello_rust.rs")], lang="rust")
    install(b, "apps/hello_rust.wap", wap)
    wap = guest(
        b,
        "tetris",
        [b.src("tetris/tetris.rs"), b.src("tetris/net_shim.c")],
        manifest=b.src("tetris/linker.metadata"),
        lang="rust",
    )
    install(b, "apps/tetris.wap", wap)
