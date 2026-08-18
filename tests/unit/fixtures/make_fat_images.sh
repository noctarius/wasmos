#!/usr/bin/env bash
# Build FAT images with the PLATFORM's formatter, for tests that must not read
# their own assumptions back.
#
# Every other FAT test in this tree writes its own BPB, so it can only confirm
# the driver agrees with this repository's reading of the specification. These
# images come from mkfs.vfat / newfs_msdos and are verified afterwards by
# fsck.vfat / fsck_msdos, so a disagreement between us and a real
# implementation shows up as a failure rather than as agreement with ourselves.
#
# Three volumes, because they exercise different driver paths:
#   fat16.img  8.3 short names only, fixed root region
#   vfat.img   FAT16 carrying long file names (the LFN reassembly path)
#   fat32.img  32-bit FAT entries, split start cluster, cluster-chained root
#
# Small clusters are deliberate: MANYFILES/ has to spill past its first cluster
# so the chain-walking scans are exercised against a real formatter's layout.
#
# Usage: make_fat_images.sh <output-dir>
# Exits 77 (the automake "skip" convention) when no formatter is available.
set -euo pipefail

OUT="${1:?usage: make_fat_images.sh <output-dir>}"
mkdir -p "$OUT"

say() { printf '[fat-fixtures] %s\n' "$*"; }
die() { printf '[fat-fixtures] ERROR: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1; }

OS="$(uname -s)"
case "$OS" in
    Linux)  need mkfs.vfat   || { say "mkfs.vfat not found (install dosfstools); skipping"; exit 77; }
            need mcopy       || { say "mcopy not found (install mtools); skipping"; exit 77; } ;;
    Darwin) need newfs_msdos || { say "newfs_msdos not found; skipping"; exit 77; } ;;
    *)      say "unsupported platform $OS; skipping"; exit 77 ;;
esac

# The content every image carries.  `stage` is populated once and copied in.
STAGE="$OUT/stage"
build_stage() {
    local want_lfn="$1"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/SUBDIR" "$STAGE/MANYFILES"

    printf 'hello from the root\n'       > "$STAGE/README.TXT"
    printf 'short name in a subdir\n'    > "$STAGE/SUBDIR/CHILD.TXT"
    # 64 bytes exactly, so a size assertion is unambiguous.
    printf '%064d' 0                     > "$STAGE/SIZED.BIN"
    # Overwritten with a SHORTER payload through O_TRUNC, so the external check
    # can tell a real shrink from a partial overwrite that left a tail behind.
    printf '%064d' 7                     > "$STAGE/TRUNCME.TXT"

    if [ "$want_lfn" = "1" ]; then
        printf 'a long file name\n' > "$STAGE/a-long-file-name.txt"
        printf 'mixed Case Name\n'  > "$STAGE/Mixed Case Name.txt"
    fi

    # Enough entries that MANYFILES/ cannot fit in one cluster at the cluster
    # sizes chosen below.  Each name is short, so one dirent per file.
    local i
    for i in $(seq -w 0 63); do
        printf 'f%s\n' "$i" > "$STAGE/MANYFILES/F$i.TXT"
    done
}

# --- Linux -----------------------------------------------------------------
make_image_linux() {
    local img="$1" fat="$2" size_mb="$3" cluster_sectors="$4"
    rm -f "$img"
    dd if=/dev/zero of="$img" bs=1048576 count="$size_mb" status=none
    mkfs.vfat -F "$fat" -s "$cluster_sectors" -n WASMOS "$img" >/dev/null
    local entry
    for entry in "$STAGE"/*; do
        [ -e "$entry" ] || continue
        mcopy -s -o -i "$img" "$entry" ::/ || die "mcopy failed for $entry"
    done
}

# --- macOS -----------------------------------------------------------------
make_image_macos() {
    local img="$1" fat="$2" size_mb="$3" cluster_sectors="$4"
    rm -f "$img"
    dd if=/dev/zero of="$img" bs=1048576 count="$size_mb" status=none

    local dev
    dev="$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$img" \
           | awk 'NR==1{print $1}')"
    [ -n "$dev" ] || die "hdiutil could not attach $img"
    newfs_msdos -F "$fat" -c "$cluster_sectors" -v WASMOS "$dev" >/dev/null 2>&1 \
        || { hdiutil detach "$dev" -quiet || true; die "newfs_msdos -F $fat failed"; }
    hdiutil detach "$dev" -quiet

    local mnt
    mnt="$(hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$img" \
           | grep -o '/Volumes/.*' | head -1)"
    [ -n "$mnt" ] && [ -d "$mnt" ] || die "image mounted at no discoverable path"
    # AppleDouble sidecars are written even with --norsrc --noextattr and would
    # consume long-name directory entries, so they are removed before detach.
    ditto --norsrc --noextattr --noqtn "$STAGE" "$mnt" \
        || { hdiutil detach "$mnt" -quiet || true; die "ditto failed"; }
    find "$mnt" -name '._*' -delete 2>/dev/null || true
    find "$mnt" -name '.DS_Store' -delete 2>/dev/null || true
    sync
    hdiutil detach "$mnt" -quiet
}

make_image() {
    case "$OS" in
        Linux)  make_image_linux "$@" ;;
        Darwin) make_image_macos "$@" ;;
    esac
}

# Sizes keep each volume inside its FAT type's cluster-count window while
# leaving MANYFILES/ multi-cluster: FAT16 is [4085, 65525) clusters, FAT32
# starts at 65525.
build_stage 0
say "fat16.img  (8.3 only, 1 sector/cluster)"
make_image "$OUT/fat16.img" 16 8 1

build_stage 1
say "vfat.img   (FAT16 + long file names, 1 sector/cluster)"
make_image "$OUT/vfat.img" 16 8 1

build_stage 1
say "fat32.img  (FAT32, 1 sector/cluster)"
make_image "$OUT/fat32.img" 32 64 1

rm -rf "$STAGE"
say "images ready in $OUT"
