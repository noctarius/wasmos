# Build configurations

Kconfig defconfigs for the supported (WASM runtime × CPU topology) build
combinations. Each selects the WASM runtime and CPU topology. WARP builds now
always execute through the ring-3 path; that mode is no longer separately
configurable.

| defconfig                | runtime | CPUs   | ring 3            |
|--------------------------|---------|--------|-------------------|
| `wasm3_single_defconfig` | wasm3   | single | always (wasm3)    |
| `wasm3_smp_defconfig`    | wasm3   | SMP    | always (wasm3)    |
| `warp_single_defconfig`  | WARP    | single | always (WARP)        |
| `warp_smp_defconfig`     | WARP    | SMP    | always (WARP)        |

`wasmos_defconfig` is the shared base (language toolchains, IRQ mode, etc.).

## Build and run

Each config builds into its own `build-<config>` tree. Use the helper:

```sh
scripts/run_config.sh warp_smp                 # boot WARP+SMP to the CLI
scripts/run_config.sh wasm3_smp                # boot wasm3+SMP to the CLI
scripts/run_config.sh wasm3_single kernel      # just build the kernel
```

or drive cmake directly:

```sh
cmake -S . -B build-warp-smp -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig
cmake --build build-warp-smp --target run-qemu-test
```

`run-qemu-test` is the boot-to-CLI gate. The runtime-independent SMP scheduler
stress test is a separate target on an SMP build configured with
`-DWASMOS_SCHED_SMP_STRESS=ON` (`run-qemu-sched-stress-test`):

```sh
cmake -S . -B build-sched-stress -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig \
      -DWASMOS_SCHED_SMP_STRESS=ON
cmake --build build-sched-stress --target run-qemu-sched-stress-test
```

The option is a plain CMake option rather than a Kconfig symbol, so it can also
be flipped on an existing tree with a reconfigure; only the kernel rebuilds. Run
the target without it and the gate refuses (exit 2) instead of booting, because
a kernel that cannot emit the marker is indistinguishable from a stalled ring.
