"""Top-level manifest: host tools, then components in dependency order.

Exports flow downward: a manifest may read what an earlier one published in
b.exports, so the subdir order below is the dependency order."""


def build(b):
    host = b.toolchain("host")
    packer = host.link(
        "make_wasmos_app", host.compile([b.src("scripts/make_wasmos_app.c")])
    )
    b.exports["packer"] = packer
    b.alias("make_wasmos_app", packer)

    b.exports["sdk.libdir"] = "wasmos-sdk/sysroot/lib/wasm32-unknown-wasmos"
    b.exports["sdk.archives"] = []
    b.subdir("src/libc", "src/libsys", "scripts/sdk")
    b.subdir("examples/c")
    b.subdir("src/kernel", "src/boot")
