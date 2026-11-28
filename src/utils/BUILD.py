"""System utilities (/system/utils)."""


def build(b):
    b.subdir(
        "date",
        "sched_info",
        "ps",
        "cat",
        "sysinfo",
        "explorer",
        "ip",
        "host",
        "mkdir",
        "mount",
        "umount",
        "blkinfo",
        "volinfo",
        "partprobe",
        "fsck_wfs",
        "mkfs_wfs",
        "curl",
        "dmesg",
        "svc",
        "trust",
    )
