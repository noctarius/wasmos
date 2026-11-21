#!/bin/sh
# run_config.sh - configure, build, and run a named wasmos build config.
#
# Usage: scripts/run_config.sh <config> [target]
#   <config>  one of the defconfigs in configs/ (without the _defconfig suffix):
#             wasm3_single | wasm3_smp | warp_single | warp_smp
#   [target]  ninja target to build (default: run-qemu-test). Useful targets:
#             run-qemu-test, run-qemu-sched-stress-test (needs
#             WASMOS_SCHED_SMP_STRESS), kernel, run-qemu, run-qemu-debug.
#
# Each config gets its own build directory (out/<config>), seeded once from its
# defconfig, so switching configs never reconfigures another config's tree.
#
# Examples:
#   scripts/run_config.sh warp_smp                 # boot + halt test, WARP+SMP
#   scripts/run_config.sh wasm3_smp run-qemu       # boot wasm3+SMP to the CLI
#   scripts/run_config.sh wasm3_single kernel      # just build the kernel
set -eu

cfg="${1:?usage: run_config.sh <wasm3_single|wasm3_smp|warp_single|warp_smp> [target]}"
target="${2:-run-qemu-test}"

defconfig="configs/${cfg}_defconfig"
if [ ! -f "$defconfig" ]; then
    echo "run_config.sh: unknown config '$cfg' (no $defconfig)" >&2
    echo "available:" >&2
    ls configs/*_defconfig 2>/dev/null | sed 's,configs/,  ,; s,_defconfig,,' >&2
    exit 2
fi

build="out/${cfg}"
if [ ! -f "$build/build.ninja" ]; then
    ./configure "$build" "$defconfig"
fi
exec ninja -C "$build" "$target"
