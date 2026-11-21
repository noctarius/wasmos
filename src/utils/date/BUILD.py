"""date: AssemblyScript system utility."""

from wasmos_rules import asc_tool, install


def build(b):
    install(
        b,
        "system/utils/date.wap",
        asc_tool(b, "date", b.src("date.ts"), "date_tool.wasm"),
    )
