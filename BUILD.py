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
    b.subdir("src/libc", "src/libsys", "scripts/sdk", "src/tools/warp_aot")
    b.subdir("src/drivers", "src/services", "src/utils")
    b.subdir(
        "examples/c",
        "examples/assemblyscript",
        "examples/rust",
        "examples/go",
        "examples/zig",
    )
    b.subdir("src/kernel", "src/boot", "src/tools/mkfs_wfs")
    b.alias("modules", default=True)
    b.subdir("scripts/system")
