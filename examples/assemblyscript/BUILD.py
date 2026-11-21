"""Assemblyscript example guest modules."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "hello_as",
        [b.src("hello/app.ts")],
        manifest=b.src("hello/linker.metadata"),
        lang="as",
    )
    install(b, "apps/hello_as.wap", wap)
    wap = guest(
        b,
        "minesweeper",
        [
            b.src("minesweeper/app.ts"),
            b.root / "src/libui/assemblyscript/libui.ts",
        ],
        manifest=b.src("minesweeper/linker.metadata"),
        lang="as",
    )
    install(b, "apps/minesweeper.wap", wap)
