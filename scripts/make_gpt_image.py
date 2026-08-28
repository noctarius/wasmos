#!/usr/bin/env python3
"""Build a raw disk image carrying a GPT and one FAT16 volume.

Written because a GPT disk is the thing the partition manager exists to read,
and QEMU cannot present one: `-drive file=fat:rw:<dir>` synthesises a classic
MBR with a hardcoded disk signature, whatever the guest is. Testing GPT against
a disk that never carries a GPT is not testing it.

The image is deliberately built here rather than with sgdisk/mkfs.vfat/parted.
Those are not present on every developer machine nor on every CI runner, and a
build step that silently degrades when a tool is missing is worse than no build
step. Everything below is bytes and struct.pack; the only import that matters is
zlib.crc32, which is what GPT specifies.

Layout, all LBAs at 512 bytes (UEFI 2.10 section 5.3):

    0             protective MBR, one 0xEE entry spanning the disk
    1             GPT header
    2..33         partition entry array, 128 entries of 128 bytes
    34..          the FAT16 volume
    last-33..-1   backup entry array
    last          backup GPT header

Usage:
    make_gpt_image.py --output user.img --label user [--source DIR] [--size-mib N]

Files in --source are copied into the volume's root directory. Only the root is
populated and only 8.3 short names are written: this exists to put a couple of
files on a test volume, not to be a filesystem writer.
"""

import argparse
import os
import struct
import sys
import zlib

SECTOR = 512
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_LBAS = (GPT_ENTRIES * GPT_ENTRY_SIZE) // SECTOR  # 32
GPT_HEADER_SIZE = 92

# "Microsoft basic data", the type every general-purpose data partition carries.
BASIC_DATA_GUID = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

# The FAT16 window: fewer than 4085 clusters is FAT12 and 65525 or more is FAT32,
# and a volume that lands in the wrong band is a volume this project's driver
# reads as the wrong filesystem. 2 KiB clusters over a 64 MiB partition sits near
# the middle of the band with room on both sides.
SECTORS_PER_CLUSTER = 4
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 512
FAT16_MIN_CLUSTERS = 4085
FAT16_MAX_CLUSTERS = 65524


def guid_to_bytes(text: str) -> bytes:
    """Canonical GUID text to the 16 raw bytes GPT stores.

    GPT writes a GUID MIXED-ENDIAN: the first three fields little-endian, the
    last two as written. Getting this wrong produces an image that parses
    perfectly and matches no rule, so it is done once, here.
    """
    parts = text.split("-")
    if len(parts) != 5 or [len(p) for p in parts] != [8, 4, 4, 4, 12]:
        raise ValueError(f"not a canonical GUID: {text}")
    a, b, c, d, e = (bytes.fromhex(p) for p in parts)
    return a[::-1] + b[::-1] + c[::-1] + d + e


def stable_guid(seed: str) -> bytes:
    """A GUID derived from `seed`, so rebuilding the image does not change it.

    A random disk GUID would make every build produce a different image, which
    turns a byte-identical rebuild into a diff and makes a PARTUUID rule
    unwritable. Version and variant bits are set so the result is a well-formed
    v4 GUID even though it is not random.
    """
    raw = bytearray(zlib.crc32(seed.encode()).to_bytes(4, "little") * 4)
    for i, byte in enumerate(seed.encode()[:16]):
        raw[i] ^= byte
    raw[7] = (raw[7] & 0x0F) | 0x40
    raw[8] = (raw[8] & 0x3F) | 0x80
    return bytes(raw)


def short_name(name: str) -> bytes:
    """An 8.3 name padded to 11 bytes, or ValueError.

    Refuses rather than mangles. A generated 8.3 name (`LONGFI~1.TXT`) is a name
    nobody asked for appearing in a directory listing an assertion may be reading,
    so a source file that cannot be represented is a build failure with the
    filename in it.
    """
    stem, _, ext = name.rpartition(".")
    if not stem:
        stem, ext = name, ""
    if len(stem) > 8 or len(ext) > 3 or not stem:
        raise ValueError(
            f"{name!r} is not an 8.3 name; rename it or extend this script"
        )
    upper = (stem + ext).upper()
    if any(c in upper for c in '"*+,/:;<=>?[\\]|'):
        raise ValueError(f"{name!r} contains a character FAT reserves")
    return (stem.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii")


def dir_entry(name11: bytes, attr: int, first_cluster: int, size: int) -> bytes:
    """One 32-byte FAT directory entry.

    Timestamps are left at zero. They are not read by anything in this tree and a
    build-time clock would make the image differ between builds.
    """
    return struct.pack(
        "<11sBBBHHHHHHHI",
        name11,
        attr,
        0,  # NT reserved
        0,  # creation time, tenths
        0,  # creation time
        0,  # creation date
        0,  # last access date
        0,  # first cluster high (FAT32 only)
        0,  # write time
        0,  # write date
        first_cluster,
        size,
    )


def build_fat16(total_sectors: int, label: str, files: list) -> bytes:
    """A FAT16 volume of `total_sectors`, holding `files` in its root.

    `files` is a list of (name, bytes). Every file gets a contiguous run of
    clusters: there is no fragmentation to model when the volume is written once
    from empty, and a contiguous allocator is a page of code instead of a
    filesystem.
    """
    cluster_sectors = SECTORS_PER_CLUSTER
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR

    # fat_size has to be chosen before the cluster count is known, because the
    # cluster count depends on it. Iterate: the answer moves at most a sector or
    # two and settles immediately.
    fat_sectors = 1
    for _ in range(8):
        data_sectors = total_sectors - (
            RESERVED_SECTORS + FAT_COUNT * fat_sectors + root_sectors
        )
        clusters = data_sectors // cluster_sectors
        needed = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if needed == fat_sectors:
            break
        fat_sectors = needed
    else:
        raise ValueError("FAT size did not converge")

    data_sectors = total_sectors - (
        RESERVED_SECTORS + FAT_COUNT * fat_sectors + root_sectors
    )
    clusters = data_sectors // cluster_sectors
    if not FAT16_MIN_CLUSTERS <= clusters <= FAT16_MAX_CLUSTERS:
        raise ValueError(
            f"{clusters} clusters is outside the FAT16 band "
            f"[{FAT16_MIN_CLUSTERS}, {FAT16_MAX_CLUSTERS}]; change the partition size "
            "or SECTORS_PER_CLUSTER"
        )

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xeb\x3c\x90"  # jmp short, the shape a BPB is expected to open with
    boot[3:11] = b"WASMOS  "
    struct.pack_into(
        "<HBHBHHBHHHII",
        boot,
        11,
        SECTOR,  # bytes per sector
        cluster_sectors,
        RESERVED_SECTORS,
        FAT_COUNT,
        ROOT_ENTRIES,
        0 if total_sectors > 0xFFFF else total_sectors,  # total_sectors_16
        0xF8,  # media descriptor: fixed disk
        fat_sectors,
        63,  # sectors per track, for BIOSes that still look
        255,  # heads
        0,  # hidden sectors: the volume is addressed from its own LBA 0
        total_sectors if total_sectors > 0xFFFF else 0,  # total_sectors_32
    )
    struct.pack_into(
        "<BBBI11s8s",
        boot,
        36,
        0x80,  # drive number
        0,  # reserved
        0x29,  # extended boot signature: the three fields below are present
        zlib.crc32(label.encode()),  # volume serial
        label.upper().ljust(11).encode("ascii")[:11],
        b"FAT16   ",
    )
    boot[510:512] = b"\x55\xaa"

    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)  # media byte + end-of-chain

    root = bytearray(root_sectors * SECTOR)
    root[0:32] = dir_entry(label.upper().ljust(11).encode("ascii")[:11], 0x08, 0, 0)
    entry_at = 32

    data = bytearray(data_sectors * SECTOR)
    next_cluster = 2
    cluster_bytes = cluster_sectors * SECTOR

    for name, content in files:
        if entry_at + 32 > len(root):
            raise ValueError(f"root directory full at {name!r}")
        span = max(1, (len(content) + cluster_bytes - 1) // cluster_bytes)
        if next_cluster + span - 1 > clusters + 1:
            raise ValueError(f"{name!r} does not fit in the volume")
        first = next_cluster
        for i in range(span):
            here = first + i
            link = 0xFFFF if i == span - 1 else here + 1
            struct.pack_into("<H", fat, here * 2, link)
        offset = (first - 2) * cluster_bytes
        data[offset : offset + len(content)] = content
        root[entry_at : entry_at + 32] = dir_entry(
            short_name(name), 0x20, first, len(content)
        )
        entry_at += 32
        next_cluster += span

    return bytes(boot) + bytes(fat) * FAT_COUNT + bytes(root) + bytes(data)


def gpt_header(
    my_lba, alt_lba, first_usable, last_usable, entry_lba, disk_guid, entries_crc
):
    header = bytearray(GPT_HEADER_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        b"EFI PART",
        0x00010000,  # revision 1.0
        GPT_HEADER_SIZE,
        0,  # header CRC32, filled in below over exactly header_size bytes
        0,  # reserved
        my_lba,
        alt_lba,
        first_usable,
        last_usable,
        disk_guid,
        entry_lba,
        GPT_ENTRIES,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    struct.pack_into("<I", header, 16, zlib.crc32(bytes(header)))
    return bytes(header).ljust(SECTOR, b"\0")


def build_image(size_mib: int, label: str, files: list) -> bytes:
    total_lbas = (size_mib * 1024 * 1024) // SECTOR
    # 1 header + 32 entry LBAs at each end, plus LBA 0 for the protective MBR.
    first_usable = 2 + GPT_ENTRY_LBAS
    last_usable = total_lbas - 1 - GPT_ENTRY_LBAS - 1

    # Align the volume to 1 MiB, the alignment every partitioner has used since
    # 4 KiB-sector disks arrived. Nothing here requires it; an image that looks
    # unlike every other GPT disk invites the reader to wonder why.
    part_first = (
        ((first_usable * SECTOR + 1024 * 1024 - 1) // (1024 * 1024))
        * (1024 * 1024)
        // SECTOR
    )
    part_last = last_usable
    part_sectors = part_last - part_first + 1

    volume = build_fat16(part_sectors, label, files)

    entry = bytearray(GPT_ENTRY_SIZE)
    entry[0:16] = guid_to_bytes(BASIC_DATA_GUID)
    entry[16:32] = stable_guid(f"wasmos:{label}")
    struct.pack_into("<QQQ", entry, 32, part_first, part_last, 0)
    entry[56:128] = label.encode("utf-16-le").ljust(72, b"\0")[:72]

    entries = bytes(entry) + b"\0" * (GPT_ENTRY_SIZE * (GPT_ENTRIES - 1))
    entries_crc = zlib.crc32(entries)
    disk_guid = stable_guid(f"wasmos-disk:{label}")

    mbr = bytearray(SECTOR)
    # One 0xEE entry covering the disk. Its purpose is to stop a tool that reads
    # only MBRs from believing the disk is empty and offering to partition it.
    struct.pack_into(
        "<BBBBBBBBII",
        mbr,
        446,
        0x00,  # not bootable
        0x00,
        0x02,
        0x00,  # CHS start, the conventional 0/0/2
        0xEE,  # GPT protective
        0xFF,
        0xFF,
        0xFF,  # CHS end, saturated
        1,
        min(total_lbas - 1, 0xFFFFFFFF),
    )
    mbr[510:512] = b"\x55\xaa"

    backup_entries_lba = total_lbas - 1 - GPT_ENTRY_LBAS
    primary = gpt_header(
        1, total_lbas - 1, first_usable, last_usable, 2, disk_guid, entries_crc
    )
    backup = gpt_header(
        total_lbas - 1,
        1,
        first_usable,
        last_usable,
        backup_entries_lba,
        disk_guid,
        entries_crc,
    )

    image = bytearray(total_lbas * SECTOR)
    image[0:SECTOR] = mbr
    image[SECTOR : 2 * SECTOR] = primary
    image[2 * SECTOR : 2 * SECTOR + len(entries)] = entries
    image[part_first * SECTOR : part_first * SECTOR + len(volume)] = volume
    image[backup_entries_lba * SECTOR : backup_entries_lba * SECTOR + len(entries)] = (
        entries
    )
    image[(total_lbas - 1) * SECTOR : total_lbas * SECTOR] = backup
    return bytes(image)


def verify(image: bytes) -> None:
    """Re-read what was just written, as an independent reader would.

    A builder that only checks its own intermediate values proves nothing about
    the bytes on disk; the checks below start from the image and recompute both
    CRCs the way the partition manager does.
    """
    if image[510:512] != b"\x55\xaa":
        raise ValueError("protective MBR signature missing")
    if image[SECTOR : SECTOR + 8] != b"EFI PART":
        raise ValueError("GPT signature missing")
    header = bytearray(image[SECTOR : SECTOR + GPT_HEADER_SIZE])
    stored = struct.unpack_from("<I", header, 16)[0]
    struct.pack_into("<I", header, 16, 0)
    if zlib.crc32(bytes(header)) != stored:
        raise ValueError("GPT header CRC does not verify")
    entry_lba, count, size, entries_crc = struct.unpack_from(
        "<QIII", image, SECTOR + 72
    )
    start = entry_lba * SECTOR
    if zlib.crc32(image[start : start + count * size]) != entries_crc:
        raise ValueError("GPT entry array CRC does not verify")
    first, last = struct.unpack_from("<QQ", image, start + 32)
    boot = image[first * SECTOR : first * SECTOR + SECTOR]
    if boot[510:512] != b"\x55\xaa":
        raise ValueError("no boot signature at the partition start")
    if struct.unpack_from("<H", boot, 11)[0] != SECTOR:
        raise ValueError("partition does not start with a 512-byte BPB")
    if last <= first:
        raise ValueError("partition ends before it starts")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True)
    ap.add_argument("--label", default="user")
    ap.add_argument("--size-mib", type=int, default=64)
    ap.add_argument(
        "--source", default="", help="directory whose files land in the volume root"
    )
    args = ap.parse_args(argv)

    files = []
    if args.source and os.path.isdir(args.source):
        for name in sorted(os.listdir(args.source)):
            path = os.path.join(args.source, name)
            if not os.path.isfile(path) or name.startswith("."):
                continue
            with open(path, "rb") as handle:
                files.append((name, handle.read()))

    image = build_image(args.size_mib, args.label, files)
    verify(image)

    # Written via a temporary and renamed, so a build interrupted mid-write
    # leaves the previous image intact instead of a truncated one QEMU would
    # boot from.
    tmp = args.output + ".tmp"
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(tmp, "wb") as handle:
        handle.write(image)
    os.replace(tmp, args.output)
    print(
        f"make_gpt_image: {args.output} "
        f"({args.size_mib} MiB, GPT, FAT16 labelled {args.label!r}, {len(files)} file(s))"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
