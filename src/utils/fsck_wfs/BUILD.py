"""fsck_wfs: system utility guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "fsck_wfs",
        [
            b.src("fsck_wfs.c"),
            b.src("wfs_fsck.c"),
            b.root / "src/drivers/fs/wfs/wfs_super.c",
            b.root / "src/drivers/fs/wfs/wfs_crc32c.c",
        ],
        flags=[f"-I{b.dir}", f"-I{b.root / 'src/drivers/fs/wfs'}"],
    )
    install(b, "system/utils/fsck.wfs.wap", wap)
