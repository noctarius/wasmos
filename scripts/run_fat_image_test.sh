#!/usr/bin/env bash
# Drive fs_fat against images built by the PLATFORM's formatter, then hand the
# results to the platform's checker.
#
#   1. build fat16 / vfat / fat32 images with mkfs.vfat or newfs_msdos
#   2. mount + read + MODIFY each with tests/unit/test_fat_image
#   3. check the modified image with fsck.vfat / fsck_msdos
#   4. mount the modified image on the HOST and confirm the entries the driver
#      created are visible to the operating system
#
# Step 3 is why this exists. `fsck_msdos -n` and `fsck.vfat -n` exit 0 even when
# they report a problem -- they only refuse to FIX it -- so the exit status
# means nothing here and the output is what has to be judged. That is not a
# nicety: the first run of this script exited 0 while reporting a real defect
# ("`..' entry in /MADEDIR has non-zero start cluster").
#
# Usage: run_fat_image_test.sh [--keep]
# Exits 77 (skip) when no formatter is available for this platform.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WASMOS_FAT_IMAGE_WORK:-$REPO_ROOT/build/fat-images}"
TEST_BIN="${WASMOS_FAT_IMAGE_TEST_BIN:-$REPO_ROOT/build/test_fat_image}"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

say()  { printf '[fat-image] %s\n' "$*"; }
fail() { printf '[fat-image] FAIL: %s\n' "$*" >&2; FAILURES=$((FAILURES + 1)); }
FAILURES=0

[ -x "$TEST_BIN" ] || { printf '[fat-image] ERROR: %s not built\n' "$TEST_BIN" >&2; exit 1; }

mkdir -p "$WORK"
set +e
bash "$REPO_ROOT/tests/unit/fixtures/make_fat_images.sh" "$WORK"
rc=$?
set -e
if [ "$rc" -eq 77 ]; then
    say "no formatter for this platform; skipping"
    exit 77
fi
[ "$rc" -eq 0 ] || { printf '[fat-image] ERROR: building images failed\n' >&2; exit 1; }

# --- 3. the platform's checker ---------------------------------------------
# Anything fsck calls a Warning or an Error is a failure here, because both
# tools report and then exit 0 under -n.  FSInfo free-space drift is allowed
# through by name: FSI_Free_Count is a hint the specification lets a driver
# leave stale, and treating it as a failure would flag a volume that is
# structurally sound (tracked in docs/TASKS.md).
check_image() {
    local img="$1" out
    if command -v fsck.vfat >/dev/null 2>&1; then
        out="$(fsck.vfat -n "$img" 2>&1 || true)"
    elif command -v fsck_msdos >/dev/null 2>&1; then
        out="$(fsck_msdos -n "$img" 2>&1 || true)"
    else
        say "no fsck available; cannot check $(basename "$img")"
        return 0
    fi

    local bad
    bad="$(printf '%s\n' "$out" \
           | grep -E '^(Warning|Error)' \
           | grep -v 'Free space in FSInfo block' \
           | grep -vE '^Warning: [0-9]+ files,' || true)"
    if [ -n "$bad" ]; then
        fail "fsck rejected $(basename "$img"):"
        printf '%s\n' "$bad" | sed 's/^/         /' >&2
        return 1
    fi
    say "fsck accepts $(basename "$img")"
    return 0
}

# --- 4. can the host itself read what the driver wrote? ---------------------
# The strongest check available: not "a checker found no fault" but "the
# operating system mounts it and sees the entries".
verify_host_readable() {
    local img="$1" name
    name="$(basename "$img")"

    if command -v mdir >/dev/null 2>&1; then
        local listing
        listing="$(mdir -i "$img" ::/ 2>&1 || true)"
        printf '%s' "$listing" | grep -q 'WROTE' \
            || { fail "$name: mtools does not see the created file"; return 1; }
        printf '%s' "$listing" | grep -q 'MADEDIR' \
            || { fail "$name: mtools does not see the created directory"; return 1; }
        listing="$(mdir -i "$img" ::/MADEDIR 2>&1 || true)"
        printf '%s' "$listing" | grep -q 'INNER' \
            || { fail "$name: mtools does not see the nested file"; return 1; }
        say "mtools reads $name"
        return 0
    fi

    if [ "$(uname -s)" = "Darwin" ]; then
        local mnt
        mnt="$(hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$img" \
               2>/dev/null | grep -o '/Volumes/.*' | head -1)"
        if [ -z "$mnt" ] || [ ! -d "$mnt" ]; then
            fail "$name: the host refused to mount the modified image"
            return 1
        fi
        local ok=1
        [ -f "$mnt/WROTE.TXT" ]        || { fail "$name: WROTE.TXT not visible to the host"; ok=0; }
        [ -d "$mnt/MADEDIR" ]          || { fail "$name: MADEDIR not visible to the host"; ok=0; }
        [ -f "$mnt/MADEDIR/INNER.TXT" ] || { fail "$name: MADEDIR/INNER.TXT not visible"; ok=0; }
        # The formatter's own content must survive our writes.
        [ -f "$mnt/README.TXT" ]       || { fail "$name: the formatter's README.TXT was lost"; ok=0; }
        hdiutil detach "$mnt" -quiet || true
        [ "$ok" = "1" ] && say "the host mounts and reads $name"
        return 0
    fi

    say "no host reader available for $name"
    return 0
}

run_one() {
    local name="$1" type="$2" lfn="${3:-}"
    local img="$WORK/$name.img"

    say "--- $name ($type${lfn:+, $lfn}) ---"
    if ! "$TEST_BIN" "$img" "$type" $lfn; then
        fail "$name: the driver's own assertions failed"
        return
    fi
    check_image "$img" || true
    verify_host_readable "$img" || true
}

run_one fat16 fat16
run_one vfat  fat16 lfn
run_one fat32 fat32

[ "$KEEP" = "1" ] || rm -f "$WORK"/*.img

if [ "$FAILURES" -ne 0 ]; then
    printf '[fat-image] %d failure(s)\n' "$FAILURES" >&2
    exit 1
fi
say "all images pass: driver, fsck, and the host agree"
