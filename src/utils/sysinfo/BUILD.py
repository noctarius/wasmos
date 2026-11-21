"""sysinfo: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "sysinfo", [b.src("sysinfo.zig")], lang="zig")
    install(b, "system/utils/sysinfo.wap", wap)
