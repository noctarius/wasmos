#!/usr/bin/env bash
# Boot the current ESP tree under Bochs instead of QEMU, as a portability check.
#
# Bochs reaches the CLI over the serial console; the script exits 0 once the
# prompt appears and non-zero on timeout or emulator panic.
#
# Requirements beyond a C++ toolchain:
#   Linux  — dosfstools (mkfs.vfat) and mtools (mcopy)
#   macOS  — none; hdiutil and newfs_msdos ship with the OS
#
# Usage: run_bochs.sh [--smp N] [--timeout SECONDS] [--quiet]
#
#   --smp N       CPU count, default 1. SMP is a compile-time Bochs option, so
#                 changing it rebuilds the emulator (a few minutes) and needs a
#                 longer --timeout: 4 CPUs reach the prompt in roughly 300s
#                 against 130s for one.
#   --timeout S   seconds to wait for the CLI prompt, default 600
#   --quiet       suppress the live serial stream
#
# Flags win over the equivalent environment variables:
#   WASMOS_ESP_DIR     ESP tree to boot            (default build/esp)
#   WASMOS_BOCHS_WORK  build + image directory     (default build/bochs)
#   WASMOS_BOCHS_SMP / WASMOS_BOCHS_TIMEOUT / WASMOS_BOCHS_QUIET
#   WASMOS_BOCHS_IMG_MB   ESP image size in MiB       (default 128)
#   WASMOS_BOCHS_CLUSTER_SECTORS  FAT cluster size in 512-byte sectors
#                                 (default 32 = 16 KiB; see the note below)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOCHS_SRC="$REPO_ROOT/libs/bochs/bochs"
ESP_DIR="${WASMOS_ESP_DIR:-$REPO_ROOT/build/esp}"
WORK="${WASMOS_BOCHS_WORK:-$REPO_ROOT/build/bochs}"
# Captured before the assignment below shadows it, so a bare `SMP=4 run_bochs.sh`
# can be reported rather than silently ignored.
INHERITED_SMP="${SMP:-}"

SMP="${WASMOS_BOCHS_SMP:-1}"
TIMEOUT="${WASMOS_BOCHS_TIMEOUT:-600}"
QUIET="${WASMOS_BOCHS_QUIET:-0}"
SMP_EXPLICIT=0
[ -n "${WASMOS_BOCHS_SMP:-}" ] && SMP_EXPLICIT=1

# Flags override the environment. SMP especially is worth spelling out on the
# command line: it is a compile-time Bochs option, so getting it wrong does not
# just misconfigure the run, it rebuilds the emulator.
while [ $# -gt 0 ]; do
    case "$1" in
        --smp)     SMP="${2:?--smp needs a CPU count}"; SMP_EXPLICIT=1; shift 2 ;;
        --smp=*)   SMP="${1#*=}"; SMP_EXPLICIT=1; shift ;;
        --timeout) TIMEOUT="${2:?--timeout needs seconds}"; shift 2 ;;
        --quiet)   QUIET=1; shift ;;
        -h|--help)
            sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) printf 'unknown argument: %s (try --help)\n' "$1" >&2; exit 2 ;;
    esac
done

case "$SMP" in
    ''|*[!0-9]*) printf '[run-bochs] ERROR: --smp must be a number, got %s\n' "$SMP" >&2; exit 2 ;;
esac

# 128 MiB at 16 KiB clusters yields ~8190 clusters, comfortably inside the FAT16
# window [4085, 65525). FAT16 is required: fs_fat detects FAT32 but serves no
# FAT-table access on it (src/drivers/fs_fat/fat_geom.c).
#
# 16 KiB clusters also keep every directory inside a single cluster. fs_fat does
# not find entries past a subdirectory's first cluster, so small enough clusters
# make /boot/apps span several and its later entries become unreadable. QEMU's
# synthesized FAT hides this by keeping those directories single-cluster.
#
# WASMOS_BOCHS_IMG_MB=64 WASMOS_BOCHS_CLUSTER_SECTORS=4 reproduces it: /boot/apps
# then occupies three clusters and chardevc.wap, which lives in the second, fails
# to spawn. Both knobs matter — cluster count must stay inside the FAT16 window
# [4085, 65525), so 2 KiB clusters need the smaller volume.
IMG_MB="${WASMOS_BOCHS_IMG_MB:-128}"
CLUSTER_SECTORS="${WASMOS_BOCHS_CLUSTER_SECTORS:-32}"

BUILD_DIR="$WORK/bochs-build"
BOCHS_BIN="$BUILD_DIR/bochs"
IMG="$WORK/esp.img"
BOCHSRC="$WORK/wasmos.bochsrc"
SERIAL="$WORK/serial.out"
BOCHS_LOG="$WORK/bochs.log"
VOLNAME=WASMOS

OS="$(uname -s)"
say() { printf '[run-bochs] %s\n' "$*"; }
die() { printf '[run-bochs] ERROR: %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------- 1. build Bochs
build_bochs() {
    # SMP is a compile-time option in Bochs, so a cached binary is only reusable
    # for the CPU count it was configured with. Without this stamp a raised
    # WASMOS_BOCHS_SMP would silently reuse a single-CPU build and only the
    # bochsrc would claim more.
    local want="smp=$SMP"
    local stamp="$BUILD_DIR/.wasmos-build-config"

    if [ -x "$BOCHS_BIN" ]; then
        if [ "$(cat "$stamp" 2>/dev/null)" = "$want" ]; then
            say "bochs already built ($want): $BOCHS_BIN"
            return
        fi
        say "rebuilding: cached build is $(cat "$stamp" 2>/dev/null || echo 'unstamped'), need $want"
    fi
    [ -f "$BOCHS_SRC/configure" ] || die "vendored Bochs not found at $BOCHS_SRC"

    # Built from a copy: libs/ is a tracked subtree and must stay clean.
    say "copying Bochs sources to $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    mkdir -p "$WORK"
    cp -R "$BOCHS_SRC" "$BUILD_DIR"

    local smp_flag=""
    [ "$SMP" -gt 1 ] && smp_flag="--enable-smp"

    local jobs
    if need nproc; then jobs="$(nproc)"
    elif need sysctl; then jobs="$(sysctl -n hw.ncpu)"
    else jobs=4
    fi

    say "configuring (smp=$SMP)"
    (
        cd "$BUILD_DIR"
        # cpu-level=6 + avx match the CPU model selected below; evex is omitted,
        # so no AVX512-capable model may be chosen.
        ./configure \
            --enable-x86-64 \
            --enable-cpu-level=6 \
            --enable-avx \
            --enable-pci \
            --enable-clgd54xx \
            --enable-long-phy-address \
            --enable-all-optimizations \
            $smp_flag \
            --with-nogui \
            --disable-docbook
    ) > "$WORK/bochs-configure.log" 2>&1 || die "configure failed, see $WORK/bochs-configure.log"

    say "building with -j$jobs (a few minutes)"
    make -C "$BUILD_DIR" -j"$jobs" > "$WORK/bochs-make.log" 2>&1 \
        || die "build failed, see $WORK/bochs-make.log"
    [ -x "$BOCHS_BIN" ] || die "build produced no bochs binary"
    printf '%s\n' "$want" > "$stamp"
    say "built $BOCHS_BIN ($want)"
}

# ------------------------------------------------------------ 2. build ESP image
# bximage creates the container; it has no filesystem support, so the FAT is laid
# down and populated per-platform below.
create_container() {
    rm -f "$IMG" "$IMG.lock"
    "$BUILD_DIR/bximage" -func=create -hd="${IMG_MB}M" -imgmode=flat -q "$IMG" >/dev/null \
        || die "bximage failed"
}

# mcopy works identically on both platforms and needs no privileges; it is the
# preferred population path wherever mtools is installed.
populate_with_mtools() {
    say "populating with mcopy"
    local entry
    for entry in "$ESP_DIR"/*; do
        [ -e "$entry" ] || continue
        mcopy -s -o -i "$IMG" "$entry" ::/ || die "mcopy failed for $entry"
    done
}

make_image_linux() {
    need mkfs.vfat || die "mkfs.vfat not found (install dosfstools)"
    need mcopy || die "mcopy not found (install mtools)"
    create_container
    say "formatting FAT16, ${CLUSTER_SECTORS} sectors/cluster"
    mkfs.vfat -F 16 -s "$CLUSTER_SECTORS" -n "$VOLNAME" "$IMG" >/dev/null \
        || die "mkfs.vfat failed"
    populate_with_mtools
}

make_image_macos() {
    create_container

    local dev
    dev="$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" \
           | awk 'NR==1{print $1}')"
    [ -n "$dev" ] || die "hdiutil could not attach $IMG"
    say "formatting FAT16 on $dev, ${CLUSTER_SECTORS} sectors/cluster"
    newfs_msdos -F 16 -c "$CLUSTER_SECTORS" -v "$VOLNAME" "$dev" >/dev/null 2>&1 \
        || { hdiutil detach "$dev" -quiet || true; die "newfs_msdos failed"; }
    hdiutil detach "$dev" -quiet

    if need mcopy; then
        populate_with_mtools
        return
    fi

    local mnt
    mnt="$(hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage "$IMG" \
           | grep -o '/Volumes/.*' | head -1)"
    [ -n "$mnt" ] && [ -d "$mnt" ] || die "image mounted at no discoverable path"
    say "populating via $mnt"
    # AppleDouble sidecars are written even with --norsrc --noextattr and consume
    # long-name directory entries, so they are removed before unmounting.
    ditto --norsrc --noextattr --noqtn "$ESP_DIR" "$mnt" \
        || { hdiutil detach "$mnt" -quiet || true; die "ditto failed"; }
    find "$mnt" -name '._*' -delete 2>/dev/null || true
    find "$mnt" -name '.DS_Store' -delete 2>/dev/null || true
    sync
    hdiutil detach "$mnt" -quiet
}

make_image() {
    [ -d "$ESP_DIR" ] || die "ESP tree not found at $ESP_DIR (build run-qemu-test first)"
    [ -f "$ESP_DIR/EFI/BOOT/BOOTX64.EFI" ] || die "$ESP_DIR has no EFI/BOOT/BOOTX64.EFI"
    case "$OS" in
        Linux)  make_image_linux ;;
        Darwin) make_image_macos ;;
        *)      die "unsupported platform: $OS" ;;
    esac
    say "esp.img ready (${IMG_MB} MiB, FAT16)"
}

# ----------------------------------------------------------------- 3. bochsrc
write_bochsrc() {
    local ovmf="$BOCHS_SRC/bios/OVMF/RELEASEX64_OVMF.fd"
    local vgabios="$BOCHS_SRC/bios/VGABIOS-lgpl/VGABIOS-lgpl-latest.bin"
    [ -f "$ovmf" ] || die "OVMF image not found at $ovmf"

    # Bochs round-robins every CPU on one host thread, switching every `quantum`
    # instructions, so more CPUs divide one interpreter's throughput rather than
    # adding any.
    #
    # Time to the CLI prompt at 4 CPUs on this ESP, by quantum: 1 = 432s,
    # 8 = 250s, 16 (the Bochs default) = 308s, 32 = 470s. The cost is U-shaped —
    # a fine interleave pays switch overhead, a coarse one makes every cross-CPU
    # spin wait longer — so 8 is used rather than the default. Override with
    # WASMOS_BOCHS_QUANTUM. The parameter exists only in an --enable-smp build.
    local quantum=""
    [ "$SMP" -gt 1 ] && quantum=", quantum=${WASMOS_BOCHS_QUANTUM:-8}"

    cat > "$BOCHSRC" <<EOF
# Generated by scripts/run_bochs.sh — edits are overwritten on the next run.

# corei7_haswell_4770 advertises POPCNT/LZCNT/BMI1/BMI2, which the WARP JIT emits
# without a CPUID guard, and stops short of AVX512, which this build cannot host.
cpu: model=corei7_haswell_4770, count=$SMP, ips=100000000, reset_on_triple_fault=0$quantum
memory: guest=512, host=512

# The 4 MiB OVMF image sits at the top of the 32-bit address space.
romimage: file=$ovmf, address=0xffc00000
vgaromimage: file=$vgabios

# fw_cfg hands OVMF the E820 map and the ACPI tables (RSDP/XSDT/FADT/MADT/HPET).
# The kernel takes the RSDP from the UEFI config table and parses the MADT itself.
fw_cfg: enabled=1

# pcivga is PCI 1234:1111 — the device the fbpci rule matches under QEMU, and the
# same BGA index/data ports at 0x1CE/0x1CF back its mode switching.
pci: enabled=1, chipset=i440fx, slot1=pcivga

ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
ata0-master: type=disk, path="$IMG", mode=flat
boot: disk

com1: enabled=1, mode=file, dev="$SERIAL"

display_library: nogui
clock: sync=none
log: $BOCHS_LOG
panic: action=report
error: action=report
EOF
    say "wrote $BOCHSRC"
}

# --------------------------------------------------------------------- 4. boot
run_bochs() {
    # A killed Bochs leaves this behind; the next run then panics "image locked"
    # during device init and boots on with no disk attached.
    rm -f "$IMG.lock" "$SERIAL" "$BOCHS_LOG"

    say "booting (timeout ${TIMEOUT}s) — serial: $SERIAL"
    # com1 goes to a file so the boot stays greppable for the success check and
    # readable after the run; a follower streams the same bytes to stdout, which
    # Bochs' remaining serial modes cannot do portably (pipe-* is Windows-only,
    # term needs a pty, socket-* needs a second reader).
    : > "$SERIAL"
    "$BOCHS_BIN" -q -f "$BOCHSRC" > "$WORK/bochs-stdout.log" 2>&1 &
    local pid=$!
    local tail_pid=""
    if [ "$QUIET" != "1" ]; then
        tail -n +1 -f "$SERIAL" &
        tail_pid=$!
    fi
    # shellcheck disable=SC2064
    trap "kill $pid $tail_pid 2>/dev/null || true" EXIT INT TERM

    local waited=0 rc=1
    while [ "$waited" -lt "$TIMEOUT" ]; do
        if [ -f "$SERIAL" ] && grep -aq 'WAMOS CLI' "$SERIAL"; then
            say "reached the CLI prompt after ${waited}s"
            rc=0
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            say "bochs exited before the prompt appeared"
            break
        fi
        sleep 2
        waited=$((waited + 2))
    done

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    if [ -n "$tail_pid" ]; then
        sleep 1  # let the follower drain what the guest wrote last
        kill "$tail_pid" 2>/dev/null || true
        wait "$tail_pid" 2>/dev/null || true
    fi
    trap - EXIT INT TERM

    # panic: action=report keeps Bochs running past device-init failures, so the
    # log is the only place an image-lock or geometry error is visible.
    if grep -q '>>PANIC<<' "$BOCHS_LOG" 2>/dev/null; then
        say "bochs reported panics:"
        grep '>>PANIC<<' "$BOCHS_LOG" | sed 's/^/    /' | head -5
        rc=1
    fi

    if grep -aq 'spawn failed\|fs read failed' "$SERIAL" 2>/dev/null; then
        say "guest reported spawn/read failures:"
        grep -a 'spawn failed\|fs read failed' "$SERIAL" | sed 's/^/    /' | head -5
        rc=1
    fi

    if [ "$rc" -ne 0 ] && [ "$QUIET" = "1" ]; then
        say "FAILED — serial tail:"
        tail -15 "$SERIAL" 2>/dev/null | sed 's/^/    /'
    fi
    return "$rc"
}

mkdir -p "$WORK"

if [ "$SMP_EXPLICIT" -eq 0 ] && [ -n "$INHERITED_SMP" ]; then
    say "note: SMP=$INHERITED_SMP in the environment is not read; use --smp $INHERITED_SMP"
fi
say "config: smp=$SMP, image=${IMG_MB}MiB, cluster=$((CLUSTER_SECTORS * 512))B, esp=$ESP_DIR"

build_bochs
make_image
write_bochsrc
run_bochs
