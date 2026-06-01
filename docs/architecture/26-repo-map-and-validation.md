## Repository Map and Validation

This document describes the repository layout, build configuration, artifact
locations, and the validation targets used to gate changes.

---

### Repository Layout

```
wasmos/
├── src/
│   ├── boot/               UEFI bootloader (single .c file + CMakeLists.txt)
│   ├── kernel/             Kernel: scheduler, IPC, memory, wasm3 hosting
│   │   ├── arch/x86_64/    CPU init, ISR stubs, context-switch, IRQ, syscall
│   │   └── include/        All kernel headers (IPC, process, paging, wasm3, etc.)
│   ├── drivers/            Device driver WASM modules
│   │   ├── ata/            ATA/PIO storage driver
│   │   ├── chardev/        Character device server/client
│   │   ├── framebuffer/    VESA framebuffer text driver
│   │   ├── framebuffer_pci/ PCI-enumerated framebuffer driver
│   │   ├── fs_fat/         FAT filesystem driver
│   │   ├── fs_init/        Initfs (read-only boot image) driver
│   │   ├── keyboard/       PS/2 keyboard driver
│   │   ├── mouse/          PS/2 mouse driver
│   │   ├── rtc/            RTC (real-time clock) driver
│   │   ├── serial/         UART serial driver
│   │   ├── virtio_serial/  VirtIO serial port driver
│   │   └── include/        Shared driver ABI headers
│   │       ├── wasmos_driver_abi.h   IPC opcode enums for all driver/service types
│   │       └── wasmos_native_driver.h Native ELF driver API
│   ├── services/           WASM user-space services
│   │   ├── acpi_bus/       ACPI bus enumerator service
│   │   ├── cli/            Interactive shell service
│   │   ├── device_manager/ Device manager + rule engine
│   │   ├── font_service/   Font rasterization service (native ELF)
│   │   ├── fs_manager/     VFS routing and mount manager
│   │   ├── gfx_compositor/ Graphics compositor (native ELF)
│   │   ├── pci_bus/        PCI bus enumerator service
│   │   ├── sysinit/        System initializer (reads sysinit.rc)
│   │   └── vt/             Virtual terminal service
│   ├── libc/               User-space libc surface and language shims
│   │   ├── src/            C implementation (startup, stdio, stdlib, etc.)
│   │   ├── include/        Standard C headers + wasmos/api.h (WASM imports)
│   │   ├── go/             Go (TinyGo) shim (wasmos.go)
│   │   ├── rust/           Rust shim (wasmos.rs)
│   │   ├── zig/            Zig shim (wasmos.zig)
│   │   └── assemblyscript/ AssemblyScript shim (runtime.ts, wasmos.ts)
│   ├── libsys/             Higher-level system library
│   │   ├── wasm/           WASM-side libsys (IPC helpers, script runner, shmem)
│   │   │   └── include/wasmos/  libsys.h, libsys_string.h, sha256.h, rtc_ipc.h
│   │   └── native/         Native (Zig) libsys for native ELF components
│   └── utils/              Small standalone utilities
│       └── date/           Date/time command (AssemblyScript)
├── examples/               Language example applications
│   ├── c/                  C hello-world
│   ├── rust/               Rust hello-world
│   ├── go/                 Go hello-world
│   ├── zig/                Zig hello-world
│   └── assemblyscript/     AssemblyScript hello-world
├── libs/                   Third-party dependencies (do not modify)
│   ├── wasm3/              wasm3 interpreter (git subtree)
│   └── stb/                stb_truetype for font rendering
├── scripts/                Build tooling and test scripts
│   ├── make_wasmos_app.c   WASMOS-APP packer tool source
│   ├── make_initfs.py      Initfs image builder
│   ├── initfs.toml         Initfs manifest (bootstrap modules)
│   ├── initfs/             Initfs data files (device-manager rules)
│   ├── system/             Runtime ESP data (rules, fonts, sysinit.rc)
│   ├── startup.nsh         UEFI shell startup script
│   ├── qemu_test_framework.py  Shared QEMU session framework
│   ├── qemu_halt_test.py       Boot+halt smoke test
│   ├── qemu_ring3_halt_test.py Ring3 isolation marker test
│   ├── qemu_ring3_fault_storm_test.py Fault-storm liveness test
│   ├── qemu_ui_test.py         UI+serial smoke test
│   └── run_unittest_suite.py   unittest discovery runner
├── tests/                  Integration and regression tests
│   ├── unit/               Host-compiled C unit tests
│   │   ├── test_list.c     Chunked/linked list data structure tests
│   │   ├── test_device_manager_rules.c  Rule parser correctness tests
│   │   └── test_fs_manager_path.c       Path normalization tests
│   ├── test_boot_smoke.py      Kernel startup marker test
│   ├── test_cli.py             CLI command integration tests
│   ├── test_device_manager.py  Device manager spawn tests
│   ├── test_fs_open_smoke.py   Filesystem open path tests
│   ├── test_fs_write_smoke.py  Filesystem write path tests
│   ├── test_hello_*.py         Language example smoke tests (C/Rust/Go/Zig/AS)
│   ├── test_chardev_preempt.py Chardev under preemption test
│   ├── test_preempt_smoke.py   Scheduler preemption smoke test
│   ├── test_ipc_wakeup.py      IPC wakeup correctness test
│   ├── test_timer_tick.py      Timer interrupt test
│   ├── test_ring3_smoke_target.py Ring3 isolation/syscall smoke
│   ├── test_threading_ipc_stress.py Thread+IPC stress test
│   ├── test_irq_route_capability.py  IRQ capability enforcement test
│   ├── test_shmem_grant_revoke_e2e.py Shmem lifecycle test
│   ├── test_vt_cli_lockup.py   VT + CLI deadlock regression test
│   └── test_make_wasmos_app_capabilities.py  Packer capability validation
├── userfs/                 Content exposed to QEMU as secondary FAT drive
├── docs/                   Architecture documentation
│   ├── ARCHITECTURE.md     Index and entry point
│   ├── STATUS.md           Implementation snapshot
│   ├── TASKS.md            Open work tracking
│   └── architecture/       Per-topic doc files (01–20)
└── CMakeLists.txt          Single top-level build file
```

---

### Toolchain Requirements

| Tool         | Required | Notes                                                               |
|--------------|----------|---------------------------------------------------------------------|
| LLVM clang   | yes      | `clang` / `clang++`; AppleClang is rejected at configure            |
| LLVM lld     | yes      | `ld.lld` for kernel; `lld-link` for UEFI PE/COFF                    |
| llvm-objcopy | yes      | PE/COFF conversion for UEFI bootloader                              |
| Python 3     | yes      | Build scripts and all test runners                                  |
| tinygo       | optional | Required for Go example; skipped if missing                         |
| rustc        | optional | Required for Rust example; requires `wasm32-unknown-unknown` target |
| zig          | optional | Required for Zig example; skipped if missing                        |
| asc          | optional | AssemblyScript compiler; install via `npm i -g assemblyscript`      |

CMake locates clang with hints in `/opt/homebrew/opt/llvm/bin`,
`/usr/local/opt/llvm/bin`, etc. Override: `-DCLANG=/path/to/llvm/bin/clang`.
AppleClang fails at configure with a fatal error.

#### Build Target Triples

| Component        | Target triple            | Notes                                         |
|------------------|--------------------------|-----------------------------------------------|
| UEFI bootloader  | `x86_64-unknown-uefi`    | PE/COFF, `-fshort-wchar -mno-red-zone`        |
| Kernel + drivers | `x86_64-elf`             | Freestanding, `-mcmodel=kernel -mno-red-zone` |
| WASM modules     | `wasm32`                 | `-Oz -nostdlib`                               |
| Rust examples    | `wasm32-unknown-unknown` | via rustc                                     |
| Zig examples     | `wasm32-freestanding`    | via zig build-exe                             |
| Go examples      | `wasm` (GOOS)            | via tinygo                                    |

---

### Primary Build Targets

```
cmake -S . -B build                 Configure
cmake --build build --target bootloader        Build BOOTX64.EFI
cmake --build build --target kernel            Build kernel.elf
cmake --build build --target make_wasmos_app   Build .wap packer tool
cmake --build build --target make_initfs       Build initfs.img + initfs_config.bin
cmake --build build --target run-qemu          Interactive QEMU boot
cmake --build build --target run-qemu-debug    QEMU with serial debug output
```

---

### Build Artifact Layout

After a full build, `build/esp/` is the QEMU FAT partition image:

```
build/esp/
├── EFI/BOOT/BOOTX64.EFI
├── kernel.elf
├── startup.nsh
├── initfs.img             (embedded by bootloader; not read by EFI shell)
├── apps/
│   ├── sysinit.wap
│   ├── cli.wap
│   ├── vt.wap
│   ├── chardevc.wap
│   ├── hello_c.wap        (if C examples enabled)
│   ├── hello_rust.wap     (if Rust enabled)
│   ├── hello_go.wap       (if Go/TinyGo enabled)
│   ├── hello_zig.wap      (if Zig enabled)
│   └── hello_as.wap       (if AssemblyScript enabled)
├── system/
│   ├── services/
│   │   ├── device_manager.wap
│   │   ├── pci_bus.wap
│   │   ├── acpi_bus.wap
│   │   ├── fs_manager.wap
│   │   ├── sysinit.wap
│   │   ├── cli.wap
│   │   ├── vt.wap
│   │   ├── gfxcomp.wap
│   │   └── fontsvc.wap
│   ├── drivers/
│   │   ├── ata.wap
│   │   └── fs_fat.wap
│   ├── fonts/
│   │   ├── roboto.ttf
│   │   ├── roboto_mono.ttf
│   │   └── roboto_serif.ttf
│   ├── devices/           (runtime device state, populated at boot)
│   └── devmgr/rules/default.rules  (copied from scripts/system/)
└── [smoke apps and test binaries as configured]
```

`build/userfs/` (or the repo `userfs/` directory) is the optional secondary
FAT drive, exposed to the VM as the `/user` mount point.

---

### Initfs Contents

`build/initfs.img` is a flat binary embedded in kernel ELF as a data section.
Its manifest is `scripts/initfs.toml`:

| Path in initfs                       | Bootstrap? | Module name       |
|--------------------------------------|------------|-------------------|
| `apps/chardev_client.wap`            | yes        | chardev_client    |
| `apps/init_smoke.wap`                | yes        | init_smoke        |
| `apps/native_call_min.wap`           | yes        | native-call-min   |
| `apps/native_call_smoke.wap`         | yes        | native-call-smoke |
| `apps/shmtgt.wap`                    | no         | shmtgt            |
| `apps/shmownr.wap`                   | no         | shmownr           |
| `system/services/pci_bus.wap`        | yes        | pci_bus           |
| `system/services/acpi_bus.wap`       | yes        | acpi_bus          |
| `system/drivers/ata.wap`             | yes        | ata               |
| `system/drivers/fs_fat.wap`          | yes        | fs_fat            |
| `system/drivers/fs_init.wap`         | yes        | fs-init           |
| `system/drivers/fbpci.wap`           | yes        | framebuffer_pci   |
| `system/services/fs_manager.wap`     | yes        | fs-manager        |
| `system/services/device_manager.wap` | yes        | device-manager    |
| `devmgr/rules/default.rules`         | data       | initfs rule file  |

Bootstrap modules (`flags = ["bootstrap"]`) are loaded by the bootloader into
the boot-info module table and are accessible before the FAT filesystem mounts.
Non-bootstrap modules are available via initfs listing after `fs-init` starts.

---

### Validation Targets

#### Pre-Commit Gate (required for runtime-affecting changes)

```
cmake --build build --target run-qemu-test
```

Runs `scripts/qemu_halt_test.py`: boots QEMU, waits for the `wamos>` prompt,
sends `halt`. Passes when the prompt appears within 120 seconds.

#### Full CLI Integration Suite

```
cmake --build build --target run-qemu-cli-test
```

Copies all artifacts to `build/esp`, sets `WASMOS_QEMU_ISOLATE_ESP=1`, then
runs `scripts/run_unittest_suite.py --start-dir tests`. This discovers and
runs all `test_*.py` files in `tests/` as Python `unittest` cases, each
driving a `QemuSession` against the fresh ESP copy.

#### Host Kernel Unit Tests

```
cmake --build build --target run-kernel-unit-tests
```

Compiles and runs three host-native C programs:

| Binary                            | Source file                              | Tests                             |
|-----------------------------------|------------------------------------------|-----------------------------------|
| `build/test_list`                 | `tests/unit/test_list.c`                 | Chunked + linked list correctness |
| `build/test_device_manager_rules` | `tests/unit/test_device_manager_rules.c` | Rule parser all four families     |
| `build/test_fs_manager_path`      | `tests/unit/test_fs_manager_path.c`      | Path normalization edge cases     |

Built with `clang -std=c11 -Wall -Wextra -Werror`. No QEMU required.

#### Ring3 Isolation Gate

```
cmake --build build --target strict-ring3
```

Runs `run-qemu-test` followed by `run-qemu-ring3-test` (in a shadow build
tree). The ring3 test asserts ~30 serial markers covering ABI correctness,
syscall enforcement, IPC ownership, fault isolation, and shared-memory
permission checks. See `11-diagnostics-status.md` for the full marker list.

#### Fault Storm Liveness

```
cmake --build build --target run-qemu-ring3-fault-storm-test
```

Runs `scripts/qemu_ring3_fault_storm_test.py` with up to 3 retry attempts.
Asserts scheduler liveness under sustained concurrent fault load and checks
that no forbidden error markers appear.

---

### Validation Constraints

**Tests must not run in parallel.** The targets share `build/esp` artifacts.
Running `run-qemu-test` and `run-qemu-cli-test` concurrently causes FAT
write conflicts and flaky failures. Always run one at a time.

**Documentation-only changes** (comments, docs, formatting, renames with no
semantic change) do not require QEMU test execution per the testing policy in
`AGENTS.md`.

**Source-text assertions are forbidden** in test files. Tests must verify
runtime behavior (outputs, state transitions, serial markers, exit codes),
not the presence of specific words or lines in source files.
