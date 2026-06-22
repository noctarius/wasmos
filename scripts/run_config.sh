#!/bin/sh
# run_config.sh - configure, build, and run a named wasmos build config.
#
# Usage: scripts/run_config.sh <config> [target]
#   <config>  one of the defconfigs in configs/ (without the _defconfig suffix):
#             wasm3_single | wasm3_smp | warp_single | warp_smp
#   [target]  cmake target to build (default: run-qemu-test). Useful targets:
#             run-qemu-test, run-qemu-sched-stress-test (needs the build below),
#             kernel, run-qemu, run-qemu-debug.
#
# Each config gets its own build directory (build-<config>) so switching configs
# never reconfigures another config's tree.
#
# Examples:
#   scripts/run_config.sh warp_smp                 # boot WARP+SMP to the CLI
#   scripts/run_config.sh wasm3_smp                # boot wasm3+SMP to the CLI
#   scripts/run_config.sh warp_smp run-qemu-test   # explicit target
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

build="build-${cfg}"
cmake -S . -B "$build" -DWASMOS_DOTCONFIG="$defconfig"
exec cmake --build "$build" --target "$target"
