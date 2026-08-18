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
# operating system mounts it and the CONTENT is what we wrote". Existence alone
# would pass on a file whose bytes never reached the disk, or whose size field
# says 0 -- so every one of these compares content, and the ones the driver
# modified are compared against the new content, not merely against "changed".
#
# Keep these strings in step with tests/unit/test_fat_image.c.
HELLO_TEXT='written by the wasmos fs_fat driver'
MODIFIED_TEXT='MODIFIED by fs_fat'
INNER_TEXT='nested content'
BIG_LEN=1500

# The same position-dependent pattern test_fat_image.c writes: byte i is
# (i * 7 + 3) mod 256. A shifted or truncated copy fails this, which a
# constant fill would not.
expected_big() {
    python3 -c "import sys; sys.stdout.buffer.write(bytes(((i*7+3)&0xFF) for i in range($BIG_LEN)))"
}

# Compare a file's whole content against a string (exact, including the newline
# the driver wrote).
expect_content() {
    local path="$1" want="$2" label="$3" name="$4"
    if [ ! -f "$path" ]; then
        fail "$name: $label is not visible to the host at all"
        return 1
    fi
    local got
    got="$(cat "$path")"
    if [ "$got" != "$want" ]; then
        fail "$name: $label content mismatch"
        printf '         want: %s\n' "$want" >&2
        printf '         got : %s\n' "$got" >&2
        return 1
    fi
    return 0
}

verify_host_readable() {
    local img="$1" name
    name="$(basename "$img")"

    if [ "$(uname -s)" != "Darwin" ] && ! command -v mcopy >/dev/null 2>&1; then
        say "no host reader available for $name"
        return 0
    fi

    # Both platforms end up comparing files in a directory: macOS mounts the
    # image, Linux copies the tree out with mtools.
    local dir cleanup=""
    if command -v mcopy >/dev/null 2>&1; then
        dir="$WORK/extract-$name"
        rm -rf "$dir"; mkdir -p "$dir"
        mcopy -s -n -i "$img" ::/ "$dir/" >/dev/null 2>&1 \
            || { fail "$name: mtools could not read the modified image"; return 1; }
    else
        dir="$(hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$img" \
               2>/dev/null | grep -o '/Volumes/.*' | head -1)"
        if [ -z "$dir" ] || [ ! -d "$dir" ]; then
            fail "$name: the host refused to mount the modified image"
            return 1
        fi
        cleanup="$dir"
    fi

    local ok=1
    # (a) the empty file exists and IS empty
    if [ -f "$dir/WROTE.TXT" ]; then
        [ ! -s "$dir/WROTE.TXT" ] || { fail "$name: WROTE.TXT should be empty"; ok=0; }
    else
        fail "$name: WROTE.TXT not visible to the host"; ok=0
    fi
    # (b) new file with content
    expect_content "$dir/HELLO.TXT" "$HELLO_TEXT" "HELLO.TXT" "$name" || ok=0
    # (c) multi-cluster file, byte for byte
    if [ -f "$dir/BIG.BIN" ]; then
        expected_big > "$WORK/big.expected"
        if ! cmp -s "$dir/BIG.BIN" "$WORK/big.expected"; then
            fail "$name: BIG.BIN differs from what the driver wrote"
            ok=0
        fi
        rm -f "$WORK/big.expected"
    else
        fail "$name: BIG.BIN not visible to the host"; ok=0
    fi
    # (d) the MODIFIED file carries the new content, not the formatter's
    expect_content "$dir/README.TXT" "$MODIFIED_TEXT" "README.TXT (modified)" "$name" || ok=0
    # (e) directory + nested file with content
    [ -d "$dir/MADEDIR" ] || { fail "$name: MADEDIR not visible to the host"; ok=0; }
    expect_content "$dir/MADEDIR/INNER.TXT" "$INNER_TEXT" "MADEDIR/INNER.TXT" "$name" || ok=0
    # (f) the deleted file is gone, and an untouched one still reads correctly
    [ ! -e "$dir/SIZED.BIN" ] || { fail "$name: the unlinked SIZED.BIN is still present"; ok=0; }
    expect_content "$dir/SUBDIR/CHILD.TXT" "short name in a subdir" \
                   "SUBDIR/CHILD.TXT (untouched)" "$name" || ok=0

    if [ -n "$cleanup" ]; then
        hdiutil detach "$cleanup" -quiet || true
    else
        rm -rf "$dir"
    fi
    [ "$ok" = "1" ] && say "the host reads $name and every byte matches"
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
