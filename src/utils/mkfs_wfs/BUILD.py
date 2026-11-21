"""mkfs_wfs: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "mkfs_wfs",
        [
            b.src("mkfs_wfs.c"),
            b.root / "src/tools/mkfs_wfs/wfs_mkfs.c",
            b.root / "src/drivers/fs_wfs/wfs_block.c",
            b.root / "src/drivers/fs_wfs/wfs_super.c",
            b.root / "src/drivers/fs_wfs/wfs_crc32c.c",
        ],
        flags=[
            f"-I{b.dir}",
            f"-I{b.root / 'src/tools/mkfs_wfs'}",
            f"-I{b.root / 'src/drivers/fs_wfs'}",
        ],
    )
    install(b, "system/utils/mkfs.wfs.wap", wap)
