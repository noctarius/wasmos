"""fs_fat: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(
        b,
        "fs_fat",
        [
            b.src("fs_fat.c"),
            b.src("fat_util.c"),
            b.src("fat_block.c"),
            b.src("fat_geom.c"),
            b.src("fat_name.c"),
            b.src("fat_alloc.c"),
            b.src("fat_dir.c"),
            b.src("fat_file.c"),
        ],
    )
    install(b, "system/drivers/fs_fat.wap", wap)
