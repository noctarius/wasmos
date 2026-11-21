"""sched_info: AssemblyScript system utility."""

from wasmos_rules import asc_tool, install


def build(b):
    install(
        b,
        "system/utils/sched_info.wap",
        asc_tool(b, "sched_info", b.src("sched_info.ts"), "sched_info.wasm"),
    )
