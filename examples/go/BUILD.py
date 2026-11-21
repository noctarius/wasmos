"""Go example guest modules."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "hello_go", [b.src("hello/hello_go.go")], lang="go")
    install(b, "apps/hello_go.wap", wap)
