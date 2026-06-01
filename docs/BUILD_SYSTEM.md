# WASMOS Build System

This document covers the CMake build system: how it is structured, what every
target type does, the helper functions available for adding new components, and
how the whole graph fits together.

---

## Contents

1. [Requirements and Tool Detection](#1-requirements-and-tool-detection)
2. [Configuration Variables and Options](#2-configuration-variables-and-options)
3. [Source Directory Variables](#3-source-directory-variables)
4. [CMake Helper Functions](#4-cmake-helper-functions)
5. [Target Types](#5-target-types)
6. [Infrastructure Targets](#6-infrastructure-targets)
7. [QEMU and Test Targets](#7-qemu-and-test-targets)
8. [Global Property Tracking](#8-global-property-tracking)
9. [ESP Layout](#9-esp-layout)
10. [Full Dependency Graph](#10-full-dependency-graph)
11. [Adding a New App or Driver](#11-adding-a-new-app-or-driver)

---

## 1. Requirements and Tool Detection

The root `CMakeLists.txt` detects all required tools at configure time and
errors out immediately if a required tool is missing.

| Tool                           | Purpose                                        | Override Variable               |
|--------------------------------|------------------------------------------------|---------------------------------|
| `clang` (LLVM, not AppleClang) | C compiler for UEFI, kernel, and WASM targets  | `-DCLANG=`                      |
| `clang++`                      | C++ compiler for WASM C++ modules              | auto-discovered next to `clang` |
| `ld.lld` / `lld`               | Linker for UEFI and kernel ELF                 | `-DLLD=`                        |
| `lld-link`                     | COFF linker for UEFI on macOS                  | `-DLLD_LINK=`                   |
| `llvm-objcopy`                 | Binary conversion (ELF → raw blob)             | `-DOBJCOPY=`                    |
| `python3`                      | `make_initfs.py`, Kconfig bridge, test runners | CMake `find_package(Python3)`   |
| `qemu-system-x86_64`           | QEMU test runner                               | must be on `PATH`               |
| OVMF firmware                  | UEFI firmware image for QEMU                   | `-DOVMF_CODE=`, `-DOVMF_VARS=`  |
| `rustc`                        | Rust → WASM32 (optional)                       | enabled by `RUST_ENABLE`        |
| `tinygo`                       | Go → WASM (optional)                           | enabled by `GO_ENABLE`          |
| `zig`                          | Zig → WASM32 or native ELF (optional)          | enabled by `ZIG_ENABLE`         |
| `asc`                          | AssemblyScript → WASM32 (optional)             | enabled by `AS_ENABLE`          |

AppleClang is explicitly rejected at configure time. On macOS, install LLVM via
Homebrew:
```sh
brew install llvm lld qemu
cmake -S . -B build -DCLANG=/opt/homebrew/opt/llvm/bin/clang
```

OVMF/EDK2 firmware candidates are searched automatically in common Homebrew and
system paths. Supply the path manually if auto-detection fails:
```sh
cmake -S . -B build -DOVMF_CODE=/path/to/edk2-x86_64-code.fd
```

---

## 2. Configuration Variables and Options

All options can be set on the `cmake` configure command line (`-DNAME=VALUE`)
or via the Kconfig flow (see README.md).

### Language Example Toggles

| Option        | Default | Description                   |
|---------------|---------|-------------------------------|
| `AS_ENABLE`   | `ON`    | Build AssemblyScript examples |
| `RUST_ENABLE` | `ON`    | Build Rust examples           |
| `GO_ENABLE`   | `ON`    | Build Go (TinyGo) examples    |
| `ZIG_ENABLE`  | `ON`    | Build Zig examples            |

Go and Zig are only built if the corresponding compiler is found at configure
time. The booleans `GO_AVAILABLE` and `ZIG_AVAILABLE` reflect this.

### Kernel Feature Flags

| Option                                | Default | Description                                                          |
|---------------------------------------|---------|----------------------------------------------------------------------|
| `WASMOS_TRACE`                        | `OFF`   | Enable verbose kernel and userland tracing (`WASMOS_TRACE=1` define) |
| `WASMOS_RING3_SMOKE`                  | `OFF`   | Spawn the ring-3 smoke probe process by default                      |
| `WASMOS_RING3_THREAD_LIFECYCLE_SMOKE` | `OFF`   | Spawn the thread lifecycle helper probe by default                   |
| `WASMOS_PM_TEST_HOOKS`                | `OFF`   | Enable process-manager test injection hooks                          |

### Process-Manager List Backend

| Option                           | Default | Description                                   |
|----------------------------------|---------|-----------------------------------------------|
| `WASMOS_PM_LIST_LINKED`          | `ON`    | Use linked-list backend for PM dynamic lists  |
| `WASMOS_PM_LIST_ARRAY_CHUNK`     | `OFF`   | Use array-chunk backend instead               |
| `WASMOS_PM_LIST_ARRAY_CHUNK_CAP` | `16`    | Capacity per chunk when array-chunk is active |

### Compiler and Linker Overrides

| Variable               | Default          | Description                        |
|------------------------|------------------|------------------------------------|
| `CLANG`                | `clang`          | C compiler path                    |
| `LLD`                  | `lld` / `ld.lld` | Linker path                        |
| `LLD_LINK`             | `lld-link`       | COFF linker for UEFI on macOS      |
| `OBJCOPY`              | `llvm-objcopy`   | Objcopy tool                       |
| `OVMF_CODE`            | auto             | Path to `OVMF_CODE.fd`             |
| `OVMF_VARS`            | auto             | Path to `OVMF_VARS.fd` (optional)  |
| `KERNEL_TARGET_TRIPLE` | `x86_64-elf`     | Clang target triple for kernel     |
| `QEMU_GDB_PORT`        | `1234`           | GDB stub port for `run-qemu-debug` |

---

## 3. Source Directory Variables

These CMake variables are set in the root `CMakeLists.txt` and used throughout
all sub-`CMakeLists.txt` files.

| Variable            | Source path               |
|---------------------|---------------------------|
| `BUILD_DIR`         | `${CMAKE_BINARY_DIR}`     |
| `BOOT_DIR`          | `src/boot`                |
| `KERNEL_DIR`        | `src/kernel`              |
| `LIBC_DIR`          | `src/libc`                |
| `LIBSYS_DIR`        | `src/libsys`              |
| `LIBSYS_WASM_DIR`   | `src/libsys/wasm`         |
| `LIBSYS_NATIVE_DIR` | `src/libsys/native`       |
| `WASM3_DIR`         | `libs/wasm3`              |
| `DRIVER_WASM_DIR`   | `src/drivers`             |
| `EXAMPLES_C_DIR`    | `examples/c`              |
| `EXAMPLES_RUST_DIR` | `examples/rust`           |
| `EXAMPLES_AS_DIR`   | `examples/assemblyscript` |
| `EXAMPLES_ZIG_DIR`  | `examples/zig`            |
| `EXAMPLES_GO_DIR`   | `examples/go`             |
| `SERVICES_DIR`      | `src/services`            |
| `USERFS_DIR`        | `userfs`                  |

---

## 4. CMake Helper Functions

All helper functions are defined in the root `CMakeLists.txt`. No separate
`.cmake` module files are used.

---

### `wasmos_add_ide_c_target(target_name)`

Creates an OBJECT library that is excluded from all normal builds
(`EXCLUDE_FROM_ALL`). Its sole purpose is to give IDEs (CLion, etc.) a
compilation unit to index for code completion and error highlighting.

Every call to `wasmos_add_wasm_c_app_target` and
`wasmos_add_native_c_app_target` automatically creates a matching `_ide` target.

**Parameters:**

| Name       | Type        | Required | Description               |
|------------|-------------|----------|---------------------------|
| `SOURCES`  | multi-value | yes      | Source files for indexing |
| `INCLUDES` | multi-value | no       | Include directories       |

---

### `wasmos_add_wasm_c_app_target(target_name)`

Compiles C sources to a WASM32 binary and packs it into a `.wap` WASMOS-APP
package. This is the primary helper for WASM drivers, services, and apps written
in C.

**Parameters:**

| Name             | Type        | Required              | Default | Description                              |
|------------------|-------------|-----------------------|---------|------------------------------------------|
| `SOURCE`         | single      | one of SOURCE/SOURCES | —       | Single C source file                     |
| `SOURCES`        | multi-value | one of SOURCE/SOURCES | —       | Multiple C source files                  |
| `OUTPUT_WASM`    | single      | yes                   | —       | Output `.wasm` path                      |
| `OUTPUT_APP`     | single      | yes                   | —       | Output `.wap` path                       |
| `MANIFEST`       | single      | yes                   | —       | Metadata manifest file                   |
| `EXPORT`         | single      | yes                   | —       | WASM export symbol (e.g. `wasmos_main`)  |
| `STACK_SIZE`     | single      | no                    | `4096`  | Stack size in bytes                      |
| `INITIAL_MEMORY` | single      | no                    | `65536` | Initial WASM memory in bytes             |
| `MAX_MEMORY`     | single      | no                    | `65536` | Maximum WASM memory in bytes             |
| `NO_BUILTIN`     | flag        | no                    | off     | Add `-fno-builtin` to the compiler flags |
| `BUILD_COMMENT`  | single      | no                    | —       | Custom status message while compiling    |
| `PACK_COMMENT`   | single      | no                    | —       | Custom status message while packing      |

**Compile flags applied:**

```
--target=wasm32 -Oz -nostdlib
-Wl,--no-entry -Wl,--strip-all
-Wl,-z,stack-size=<STACK_SIZE>
-Wl,--initial-memory=<INITIAL_MEMORY>
-Wl,--max-memory=<MAX_MEMORY>
-Wl,--allow-undefined
-Wl,--export=<EXPORT>
-I${LIBC_DIR}/include -I${LIBSYS_WASM_DIR}/include -I${DRIVER_WASM_DIR}/include
```

libc sources (`src/libc/src/*.c`) are compiled together with the app sources
in a single clang invocation.

**Side effects:**

- Appends `OUTPUT_APP` to the global property `WASMOS_WASM_APPS`.
- Appends `target_name` to the global property `WASMOS_WASM_APP_TARGETS`.
- Creates a companion `${target_name}_ide` OBJECT library.

**Example:**

```cmake
wasmos_add_wasm_c_app_target(my_driver
  SOURCE ${CMAKE_SOURCE_DIR}/src/drivers/my_driver/my_driver.c
  OUTPUT_WASM ${BUILD_DIR}/my_driver.wasm
  OUTPUT_APP  ${BUILD_DIR}/my_driver.wap
  MANIFEST    ${CMAKE_SOURCE_DIR}/src/drivers/my_driver/linker.metadata
  EXPORT      wasmos_main
  STACK_SIZE  8192
)
```

---

### `wasmos_add_wasm_cpp_app_target(target_name)`

Identical to `wasmos_add_wasm_c_app_target` but uses `clang++` and applies the
following additional C++ flags to suppress runtime-heavy features:

```
-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
```

Takes a single `SOURCE` (not `SOURCES`) argument. All other parameters and side
effects are identical to the C variant.

**Example:**

```cmake
wasmos_add_wasm_cpp_app_target(my_cpp_app
  SOURCE      ${CMAKE_SOURCE_DIR}/examples/cpp/my_app.cpp
  OUTPUT_WASM ${BUILD_DIR}/my_app.wasm
  OUTPUT_APP  ${BUILD_DIR}/my_app.wap
  MANIFEST    ${CMAKE_SOURCE_DIR}/examples/cpp/my_app.manifest.toml
  EXPORT      wasmos_main
)
```

---

### `wasmos_add_native_c_app_target(target_name)`

Compiles C sources to x86-64 ELF and packs it into a `.wap` WASMOS-APP package.
Used for drivers or services that need direct hardware access (ring-0 native
execution rather than WASM sandbox execution).

Each source file is compiled to its own `.o` object in a separate
`add_custom_command`, then all objects are linked in a second step.

**Parameters:**

| Name              | Type        | Required | Default | Description                            |
|-------------------|-------------|----------|---------|----------------------------------------|
| `SOURCES`         | multi-value | yes      | —       | C source files                         |
| `INCLUDES`        | multi-value | no       | —       | Include directories (added as `-I`)    |
| `COMPILE_FLAGS`   | multi-value | no       | —       | Extra compiler flags per source        |
| `HEADER_DEPS`     | multi-value | no       | —       | Header files that trigger a recompile  |
| `LINK_FLAGS`      | multi-value | no       | —       | Extra linker flags                     |
| `OUTPUT_ELF`      | single      | yes      | —       | Output ELF path                        |
| `OUTPUT_APP`      | single      | yes      | —       | Output `.wap` path                     |
| `MANIFEST`        | single      | yes      | —       | Metadata manifest file                 |
| `ENTRYPOINT`      | single      | yes      | —       | ELF entry point symbol                 |
| `IMAGE_BASE`      | single      | no       | —       | Optional `--image-base` for the linker |
| `COMPILE_COMMENT` | single      | no       | —       | Custom compile status message          |
| `LINK_COMMENT`    | single      | no       | —       | Custom link status message             |
| `PACK_COMMENT`    | single      | no       | —       | Custom pack status message             |

The linker is invoked directly (`lld`) with `-static -e <ENTRYPOINT>`.

**Side effects:** identical to `wasmos_add_wasm_c_app_target`.

---

## 5. Target Types

### 5.1 Bootloader (`bootloader`)

**Source:** `src/boot/CMakeLists.txt`  
**Output:** `build/BOOTX64.EFI`  
**Target triple:** `x86_64-unknown-uefi`

The bootloader is compiled in a single clang invocation targeting the UEFI PE
format. It loads `kernel.elf` and `initfs.img` from the ESP, collects the UEFI
memory map, exits boot services, and jumps to `_start` in the kernel.

Compiler flags (`CFLAGS_EFI`):
```
-ffreestanding -fno-stack-protector -fno-builtin
-target x86_64-unknown-uefi -fshort-wchar -mno-red-zone
```

Linker flags (`LDFLAGS_EFI`): COFF subsystem `efi_application`, entry `efi_main`
(uses `lld-link` on macOS, `lld` with `--flavor link` on other platforms).

---

### 5.2 Kernel (`kernel`)

**Source:** `src/kernel/CMakeLists.txt`  
**Output:** `build/kernel.elf`  
**Target triple:** `x86_64-elf` (overridable via `KERNEL_TARGET_TRIPLE`)

Each kernel source file is compiled individually to a `.o` object, then all
objects are linked by `ld.lld` against `src/kernel/arch/x86_64/linker.ld`.

Compiler flags (`CFLAGS_KERNEL`):
```
-ffreestanding -fno-stack-protector -fno-builtin
-target x86_64-elf -mno-red-zone -mcmodel=kernel -m64 -fno-jump-tables
-DWASMOS_ENABLE_PREEMPT_GUARD=1 -DWASMOS_ENABLE_SAFEPOINT=1
```

The kernel also embeds several **blob objects** that are compiled separately and
linked in as raw binary data:

- `chardev_server_blob.o` — the chardev WASM driver, loaded directly without a
  filesystem
- `ring3_native_probe_blob.o` — a minimal ring-3 ELF probe for isolation testing
- `ring3_thread_lifecycle_probe_blob.o` — ring-3 ELF probe for thread lifecycle
  testing

Each blob is produced by compiling a standalone C/ELF target, converting it to a
raw binary with `llvm-objcopy`, then wrapping the binary in a linkable `.o`
object file.

The WASM3 interpreter sources (`libs/wasm3/source/m3_*.c`) are compiled with
kernel-specific flags and linked in directly. The `libs/wasm3` subtree must
never be modified.

---

### 5.3 WASM C App (via `wasmos_add_wasm_c_app_target`)

**Output:** `.wasm` → `.wap`  
**Runtime:** executed by wasm3 inside the process manager

Applies to: most drivers in `src/drivers/`, all services in `src/services/`
(except the native Zig ones), and all C examples in `examples/c/`.

The `.wasm` binary must export the `wasmos_main` symbol (or another symbol
specified by `EXPORT`). The process manager calls this export to start the
component.

Memory layout is fixed at configure time via `INITIAL_MEMORY` / `MAX_MEMORY`.
Default is 64 KiB (one WASM page). Graphics-heavy apps use 8 MiB.

---

### 5.4 WASM C++ App (via `wasmos_add_wasm_cpp_app_target`)

Same runtime model as the C variant. The only differences are:

- `clang++` is used instead of `clang`.
- C++ runtime features that require dynamic allocation or global constructors
  are disabled: `-fno-exceptions -fno-rtti -fno-threadsafe-statics
  -fno-use-cxa-atexit`.
- Accepts only a single `SOURCE` argument.

---

### 5.5 Native C App (via `wasmos_add_native_c_app_target`)

**Output:** `.elf` → `.wap`  
**Runtime:** loaded and executed directly as ring-0 code (no WASM sandbox)

Used for drivers that need direct hardware access: `framebuffer`,
`framebuffer_pci`. The ELF is a static freestanding binary. The process manager
identifies it as a native payload through the manifest metadata and maps it
directly rather than passing it to wasm3.

---

### 5.6 AssemblyScript App

**Source:** `examples/assemblyscript/CMakeLists.txt`, `src/drivers/serial/`,
`keyboard/`, `mouse/`, `rtc/`  
**Tool:** `asc` (AssemblyScript compiler, installed via npm)  
**Output:** `.wasm` → `.wap`

The app's TypeScript source and a shared `runtime.ts` are staged into a
temporary build directory and compiled with:

```
asc entry.ts --target release -Osize --runtime stub --noAssert
```

The packing step is the same as for C WASM apps.

---

### 5.7 Rust App

**Source:** `examples/rust/CMakeLists.txt`  
**Tool:** `rustc` (not Cargo)  
**Output:** `.wasm` → `.wap`

Compiled directly with `rustc`:

```
rustc --target=wasm32-unknown-unknown -O -C opt-level=z
      -C panic=abort -C codegen-units=1
      -C link-arg=--no-entry -C link-arg=--export=wasmos_main
      -C link-arg=--strip-all
      -C link-arg=--initial-memory=2097152
      -C link-arg=--max-memory=2097152
      --crate-type=cdylib
```

Each Rust example links against `wasmos.rs` (the Rust runtime shim).

---

### 5.8 Go App (TinyGo)

**Source:** `examples/go/CMakeLists.txt`  
**Tool:** `tinygo`  
**Output:** `.wasm` → `.wap`

TinyGo is only used if it is found at configure time (`GO_AVAILABLE`).
Sources are staged to the build directory and compiled with:

```
tinygo build -target=wasm -opt=z -no-debug -panic=trap
             -scheduler=none -gc=leaking
```

`GO111MODULE=off` is set to avoid module resolution in the staged directory.

---

### 5.9 Zig WASM App

**Source:** `examples/zig/CMakeLists.txt`  
**Tool:** `zig`  
**Output:** `.wasm` → `.wap`

Zig is only used if it is found at configure time (`ZIG_AVAILABLE`).
Sources are staged to an isolated directory (separate `--cache-dir` and
`--global-cache-dir`) and compiled with:

```
zig build-exe -target wasm32-freestanding -O ReleaseSmall
              -fno-entry -fstrip --export=wasmos_main
```

---

### 5.10 Zig Native Service

**Source:** `src/services/gfx_compositor/`, `src/services/font_service/`  
**Tool:** `zig` + `lld`  
**Output:** `.elf` → `.wap`

These services use Zig targeting `x86_64-freestanding` and link against the
native `libsys` shim. The build is four steps:

1. **Compile Zig to object** — `zig build-obj -target x86_64-freestanding`
2. **Compile `libsys_native.c` to object** — exposes the `libsys` host-call
   ABI at the C level
3. **Link both objects to ELF** — `lld -e initialize -static`
4. **Pack ELF into `.wap`** — `make_wasmos_app --manifest ...`

The ELF entrypoint is `initialize` (not `main`). The process manager loads it
as a native payload, identical to native C apps.

---

## 6. Infrastructure Targets

### `make_wasmos_app`

A host-native C executable built from `scripts/make_wasmos_app.c`. It packs a
raw WASM binary or ELF file together with a manifest file into the `.wap`
WASMOS-APP container format.

```
make_wasmos_app --manifest <file> --in <binary> --out <file.wap>
```

All app and driver build targets depend on this tool.

---

### `make_initfs`

Builds `build/initfs.img` — the initial RAM filesystem that the bootloader loads
alongside the kernel. It contains drivers, services, and smoke apps that must be
available before the FAT filesystem is mounted.

The image is assembled by `scripts/make_initfs.py` from the manifest
`scripts/initfs.toml`. `make_initfs` depends on all C example targets, all
driver targets, and all service targets.

---

### `wasm_driver_blobs`

An aggregate target that depends on all kernel-embedded blob targets
(`chardev_driver_blob`, `ring3_native_probe_blob`,
`ring3_thread_lifecycle_probe_blob`). The `kernel` target depends on
`wasm_driver_blobs` to ensure blobs are ready before linking.

---

### `kconfig-defconfig`

Seeds `build/.config` from `configs/wasmos_defconfig` and imports it into the
CMake cache via `scripts/kconfig_to_cmake.py`.

---

### `menuconfig` / `kconfiglib-menuconfig`

Opens an interactive Kconfig editor. `menuconfig` prefers a system-installed
binary (`menuconfig`, `nconfig`, `kconfig-mconf`, `mconf`) and falls back to
the Python `kconfiglib` editor if none are found.
`kconfiglib-menuconfig` always uses the Python editor.

---

### `run-kernel-unit-tests`

Compiles and runs host-native unit tests from `tests/unit/` using the system C
compiler. Current suites:

- `test_list` — kernel list data-structure correctness
- `test_device_manager_rules` — device manager rule matching
- `test_fs_manager_path` — VFS path resolution

These tests run on the host (no QEMU) and do not depend on `bootloader` or
`kernel`.

---

## 7. QEMU and Test Targets

All QEMU targets share a common setup: they copy `BOOTX64.EFI`, `kernel.elf`,
`initfs.img`, drivers, services, and app `.wap` files to `build/esp/`, then
invoke a Python test runner or QEMU directly.

QEMU targets that require OVMF are only defined when both `OVMF_CODE` and
`OVMF_VARS` are found. QEMU targets that use a ring-3 shadow tree (see below)
are always defined.

**Do not run integration QEMU targets in parallel.** They share the mutable
`build/esp/` directory and parallel runs cause file-copy conflicts.

| Target                            | Needs OVMF_VARS | Description                                                        |
|-----------------------------------|-----------------|--------------------------------------------------------------------|
| `run-qemu`                        | yes             | Interactive QEMU with serial console (nographic)                   |
| `run-qemu-debug`                  | yes             | Paused QEMU with GDB stub on `QEMU_GDB_PORT`                       |
| `run-qemu-ui`                     | yes             | Interactive QEMU with graphics window                              |
| `run-qemu-test`                   | yes             | Automated halt smoke test (`qemu_halt_test.py`)                    |
| `run-qemu-cli-test`               | yes             | Full CLI integration suite (`run_unittest_suite.py`)               |
| `run-qemu-ui-test`                | optional        | Graphics UI smoke test (`qemu_ui_test.py`)                         |
| `run-qemu-ring3-test`             | yes             | Ring-3 isolation marker test (shadow build tree)                   |
| `run-qemu-ring3-threading-test`   | yes             | Ring-3 thread lifecycle marker test (shadow tree)                  |
| `run-qemu-ring3-fault-storm-test` | yes             | Ring-3 multi-process fault-storm test (shadow tree)                |
| `strict-ring3`                    | yes             | Gate profile: `run-qemu-test` + `run-qemu-ring3-test` sequentially |
| `run-kernel-unit-tests`           | no              | Host-native unit tests (no QEMU)                                   |

### Shadow Build Trees for Ring-3 Tests

Ring-3 tests (`run-qemu-ring3-test`, `run-qemu-ring3-threading-test`,
`run-qemu-ring3-fault-storm-test`) configure and build a second, independent
CMake tree under `build/ring3/`, `build/ring3-threading/`, or
`build/ring3-fault-storm/` with specific feature flags enabled
(`-DWASMOS_RING3_SMOKE=ON -DWASMOS_PM_TEST_HOOKS=ON`). This avoids polluting
the main build tree and ensures the tested kernel binary has the probe enabled.

---

## 8. Global Property Tracking

CMake global properties are used to aggregate targets across subdirectories
without requiring each subdirectory to know about its siblings.

| Property                     | Content                                              |
|------------------------------|------------------------------------------------------|
| `WASMOS_WASM_APPS`           | List of all `.wap` output file paths                 |
| `WASMOS_WASM_APP_TARGETS`    | List of all custom targets that produce `.wap` files |
| `WASMOS_KERNEL_BLOBS`        | List of embedded blob `.o` output file paths         |
| `WASMOS_KERNEL_BLOB_TARGETS` | List of custom targets that produce kernel blobs     |

After all `add_subdirectory()` calls, the root reads these properties and
establishes the necessary `add_dependencies()` edges:

- All `WASMOS_WASM_APP_TARGETS` depend on `make_wasmos_app`.
- `wasm_driver_blobs` depends on all `WASMOS_WASM_APP_TARGETS` and
  `WASMOS_KERNEL_BLOB_TARGETS`.
- `kernel` depends on `wasm_driver_blobs`.

---

## 9. ESP Layout

QEMU targets populate `build/esp/` as a FAT filesystem image:

```
build/esp/
├── EFI/BOOT/BOOTX64.EFI       # UEFI bootloader
├── kernel.elf                 # Kernel ELF binary
├── initfs.img                 # Initial RAM filesystem
├── startup.nsh                # UEFI shell startup script
├── apps/                      # User-facing applications
│   ├── hello_c.wap
│   ├── hello_rust.wap
│   ├── hello_as.wap
│   ├── hello_zig.wap
│   ├── hello_go.wap
│   ├── sysinit.wap
│   ├── cli.wap
│   ├── vt.wap
│   └── ...smoke test apps...
├── system/
│   ├── drivers/               # Hardware drivers loaded by device-manager
│   │   ├── serial.wap
│   │   ├── keyboard.wap
│   │   ├── mouse.wap
│   │   ├── rtc.wap
│   │   ├── ata.wap
│   │   ├── fs_fat.wap
│   │   ├── framebuf.wap
│   │   └── framebuf_pci.wap
│   ├── services/              # System services
│   │   ├── sysinit.wap
│   │   ├── cli.wap
│   │   ├── vt.wap
│   │   ├── device_manager.wap
│   │   ├── pci_bus.wap
│   │   ├── acpi_bus.wap
│   │   ├── gfxcomp.wap
│   │   └── fontsvc.wap
│   ├── devmgr/rules/          # Runtime device-manager policy rules
│   ├── fonts/                 # TTF fonts for the font service
│   └── utils/                 # System utility apps (date.wap, etc.)
└── large_read.txt             # Test fixture for large-read smoke test
```

`initfs.img` contains drivers and services needed before the FAT filesystem is
available (boot phase). Hardware drivers live on the ESP and are loaded by
`device-manager` after `fs-fat` mounts `/boot`.

---

## 10. Full Dependency Graph

```
make_wasmos_app (host executable, built first)
    │
    ├── wasmos_add_wasm_c_app_target targets
    │       (drivers, services, C examples → *.wap)
    ├── wasmos_add_native_c_app_target targets
    │       (framebuffer driver → *.wap)
    ├── AssemblyScript / Rust / Go / Zig example targets
    │       (*.wap)
    └── kernel blob targets
            (chardev_server_blob.o, ring3_probe blobs)

wasm_driver_blobs
    └── depends on: all *.wap targets + all blob targets

bootloader → build/BOOTX64.EFI

kernel
    └── depends on: wasm_driver_blobs
    └── output: build/kernel.elf

make_initfs
    └── depends on: c_examples, drivers, services
    └── output: build/initfs.img

run-qemu / run-qemu-test / run-qemu-cli-test / run-qemu-debug
    └── depends on: bootloader, kernel, make_initfs,
                    c_examples, services, drivers,
                    [assemblyscript_examples], [rust_examples],
                    [go_examples], [zig_examples],
                    [system_utils]
```

---

## 11. Adding a New App or Driver

### WASM C App or Driver

1. Create `src/drivers/my_driver/` (or `src/services/`, `examples/c/`) with
   your `.c` source and a `linker.metadata` manifest.
2. Add a `CMakeLists.txt` (or extend an existing one) calling:

   ```cmake
   wasmos_add_wasm_c_app_target(my_driver
     SOURCE  ${CMAKE_SOURCE_DIR}/src/drivers/my_driver/my_driver.c
     OUTPUT_WASM ${BUILD_DIR}/my_driver.wasm
     OUTPUT_APP  ${BUILD_DIR}/my_driver.wap
     MANIFEST    ${CMAKE_SOURCE_DIR}/src/drivers/my_driver/linker.metadata
     EXPORT      wasmos_main
   )
   ```
3. If it is a new subdirectory, add `add_subdirectory(src/drivers/my_driver)`
   to `src/drivers/CMakeLists.txt`.
4. Reference the `OUTPUT_APP` variable in the relevant QEMU target copy
   commands in the root `CMakeLists.txt` and add it to `initfs.toml` if it
   needs to be available before the FAT filesystem mounts.

### Native C Driver

Same as above, but use `wasmos_add_native_c_app_target` and set `ENTRYPOINT`
to your driver's entry function.

### WASM C++ Module

Use `wasmos_add_wasm_cpp_app_target`. Provide a single `SOURCE` file. Follow
the same steps as the C app.

### AssemblyScript, Rust, Go, or Zig App

Follow the pattern in the corresponding `examples/<lang>/CMakeLists.txt`. Each
language has slightly different staging and compiler invocation, but the final
`make_wasmos_app` packing step is the same.
