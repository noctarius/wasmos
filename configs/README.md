# Build configurations

Kconfig defconfigs for the supported (architecture × WASM runtime × CPU
topology) build combinations. Without an architecture line a defconfig is
x86_64. WARP builds
always execute through the ring-3 path; that mode is not separately
configurable.

| defconfig                | runtime | CPUs   | ring 3            |
|--------------------------|---------|--------|-------------------|
| `wasm3_single_defconfig` | wasm3   | single | always (wasm3)    |
| `wasm3_smp_defconfig`    | wasm3   | SMP    | always (wasm3)    |
| `warp_single_defconfig`  | WARP    | single | always (WARP)        |
| `warp_smp_defconfig`     | WARP    | SMP    | always (WARP)        |
| `aarch64_wasm3_single_defconfig` | wasm3 | single | aarch64 bring-up: schedules and runs wasm3 guests (`run-qemu-bringup-test`) |

`wasmos_defconfig` is the shared base (language toolchains, IRQ mode, etc.).

## Build and run

Each config builds into its own `out/<config>` directory, whose `.config` is
seeded once from the defconfig. Use the helper:

```sh
scripts/run_config.sh warp_smp                 # boot + halt test, WARP+SMP
scripts/run_config.sh wasm3_smp run-qemu       # boot wasm3+SMP to the CLI
scripts/run_config.sh wasm3_single kernel      # just build the kernel
```

or drive `configure` and `ninja` directly:

```sh
./configure out/warp_smp configs/warp_smp_defconfig
ninja -C out/warp_smp run-qemu-test
```

A defconfig is read only when a build directory is first configured (or with
`./configure out/<dir> configs/<name>_defconfig --reseed`); afterwards the
directory's own `.config` is the configuration. Edit it with
`ninja -C out/<dir> menuconfig` or `./configure out/<dir> --set SYMBOL=VALUE`;
`build.ninja` regenerates itself on the next build.

`run-qemu-test` is the boot-to-CLI gate. The runtime-independent SMP scheduler
stress test is a separate target on an SMP build with the Kconfig symbol
`WASMOS_SCHED_SMP_STRESS` enabled (`run-qemu-sched-stress-test`):

```sh
./configure out/warp_smp configs/warp_smp_defconfig
./configure out/warp_smp --set WASMOS_SCHED_SMP_STRESS=y
ninja -C out/warp_smp run-qemu-sched-stress-test
```

Flipping the symbol on an existing tree rebuilds only the kernel. Run
the target without it and the gate refuses (exit 2) instead of booting, because
a kernel that cannot emit the marker is indistinguishable from a stalled ring.
