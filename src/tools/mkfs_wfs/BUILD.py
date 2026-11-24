"""mkfs_wfs: host-native WFS formatter, built from the same layout sources the
wasm driver reads through (wfs_format.h, wfs_crc32c.c, wfs_super.c)."""


def build(b):
    fs_wfs = b.root / "src/drivers/fs/wfs"
    host = b.toolchain("host")
    objs = host.compile(
        [
            b.src("main.c"),
            b.src("wfs_mkfs.c"),
            fs_wfs / "wfs_crc32c.c",
            fs_wfs / "wfs_super.c",
        ],
        flags=[
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{b.dir}",
            f"-I{fs_wfs}",
            f"-I{b.root / 'abi/generated/c'}",
        ],
        subdir="obj/mkfs_wfs",
    )
    b.exports["mkfs_wfs"] = host.link("mkfs_wfs", objs)
