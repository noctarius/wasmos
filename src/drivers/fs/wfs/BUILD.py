"""fs_wfs: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "fs_wfs",
        [
            b.src("fs_wfs.c"),
            b.src("wfs_crc32c.c"),
            b.src("wfs_super.c"),
            b.src("wfs_bitmap.c"),
            b.src("wfs_dirent.c"),
            b.src("wfs_fd.c"),
            b.src("wfs_ops.c"),
            b.src("wfs_block.c"),
            b.src("wfs_mount.c"),
            b.src("wfs_journal.c"),
            b.src("wfs_recover.c"),
            b.src("wfs_extent.c"),
            b.src("wfs_extent_write.c"),
            b.src("wfs_dir.c"),
            b.src("wfs_path.c"),
            b.src("wfs_read.c"),
            b.src("wfs_write.c"),
            b.src("wfs_truncate.c"),
            b.src("wfs_alloc.c"),
            b.src("wfs_sync.c"),
            b.src("wfs_namespace.c"),
        ],
    )
    install(b, "system/drivers/fs_wfs.wap", wap)
