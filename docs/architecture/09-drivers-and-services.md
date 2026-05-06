## Drivers and Services

### Implemented Drivers
- `ata`
  - PIO ATA block driver
  - owns the `block` endpoint
  - supports identify and read operations
- `fs-fat`
  - FAT12/16/32 filesystem driver
  - consumes the `block` endpoint
  - owns the `fs` endpoint
  - supports root/subdirectory listing, `cat`, `cd`, PM app loading, and the
    minimal shared libc read-only file API
  - follows FAT12/16 cluster chains for multi-cluster file reads on the
    current ESP baseline
- `chardev`
  - IPC-backed console/character device service
- `framebuffer`
  - optional native C driver packed as `FLAG_DRIVER|FLAG_NATIVE`
  - probes the kernel framebuffer APIs exposed via GOP
  - validates geometry and maps framebuffer pages into a fixed driver device
    virtual region through the native-driver API
  - paints a gradient on the standard QEMU VGA framebuffer when the device is present

### Implemented Services
- `process-manager`
  - validates WASMOS-APP containers
  - creates process/runtime state
  - resolves required endpoints
  - starts entries
  - exposes `spawn`, `spawn by name`, `wait`, `kill`, and `status`
- `hw-discovery`
  - scans ACPI RSDP data
  - starts the early storage driver chain
  - starts post-FAT display/input drivers by name from disk
- `sysinit`
  - intentionally narrow
  - starts post-FAT services and late user processes from the generated boot
    config
- `cli`
  - interactive shell over `proc` and `fs`

### Driver and Service Startup Chain
Current startup chain:
1. bootloader loads `initfs.img`
2. initfs contributes bootstrap `boot_module_t` entries for `hw-discovery`,
   `ata`, `fs-fat`, and the current smoke/bootstrap apps
3. kernel `init` spawns `hw-discovery`
4. `hw-discovery` starts the storage chain: `ata` and `fs-fat`
5. `hw-discovery` starts post-FAT hardware drivers by name: `serial`,
   `keyboard`, and `framebuffer`
6. kernel `init` waits for a successful FAT readiness probe
7. kernel `init` loads `sysinit` from disk via PM
8. `sysinit` loads the configured `sysinit.spawn` services/processes from disk,
   including `vt` and `cli`

This is the current stable bootstrap baseline.

### Boot Config
The initial config channel is a simple binary blob generated from TOML at build
time. The current generator reads `scripts/initfs.toml` and emits both the
initfs image and a compact `bootcfg.bin` payload.

Current config format:
- magic `WCFG0001`
- version
- bootstrap-module count
- sysinit-spawn count
- string-table size
- offset arrays for each string list
- NUL-terminated ASCII string table

Current `sysinit.spawn` validation:
- at least one late-start process must be configured
- process names must be unique
- process names must fit the current 16-byte PM by-name spawn ABI

Current use:
- the blob is carried in initfs for stable packaging
- the bootloader exposes the blob through `boot_info_t`
- wasm processes can read it through `wasmos_boot_config_size()` and
  `wasmos_boot_config_copy()`
- `sysinit` validates and consumes the `sysinit.spawn` string list for its
  late-start process policy and halts that policy path if the config is
  malformed

### What Is Still Missing
- driver-manager
- full PCI enumeration
- richer ACPI-based device inventory publication
- hotplug handling
- capability-based MMIO/PIO/DMA/IRQ grants

