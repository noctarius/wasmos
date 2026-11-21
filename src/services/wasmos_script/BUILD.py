"""wasmos_script: .rc script executor service, plus its sample script."""

from wasmos_rules import guest


def build(b):
    guest(b, "wamos_script", [b.src("wamos_script.c")])
    # hello.rc is an initfs payload, resolved by name from the build directory.
    b.command(
        ["hello.rc"],
        ["cp", b.src("hello.rc"), "hello.rc"],
        inputs=[b.src("hello.rc")],
        description="COPY hello.rc",
    )
