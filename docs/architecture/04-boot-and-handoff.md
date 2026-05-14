## Boot Contract

### Bootloader Responsibilities
Intent: small, deterministic, and policy-light.

The bootloader must:
- Locate and read `kernel.elf` from the EFI System Partition.
- Locate and read `initfs.img` from the EFI System Partition.
- Validate the ELF header and load all `PT_LOAD` segments, including
  misaligned physical addresses and overlapping segment reuse.
- Collect the UEFI memory map and copy it into kernel-owned pages before
  `ExitBootServices()`.
- Fill a versioned `boot_info_t`.
- Validate the initfs header and entry table, then copy the initfs blob into
  boot handoff memory.
- Synthesize `boot_module_t` records for bootstrap-marked initfs WASMOS apps so
  the existing early-kernel bootstrap path can stay unchanged.
- Transfer control to the kernel entry point without embedding higher-level OS
  policy.

`BOOTX64.EFI` now also captures the GOP framebuffer when available. When the
GOP handles are absent, it scans VGA PCI BARs for a framebuffer, logs the
cartographic results, and pipes the discovered base, size, resolution, stride,
and `BOOT_INFO_FLAG_GOP_PRESENT` into `boot_info_t` so lower-privilege drivers
can map the framebuffer themselves.

### Kernel Entry Responsibilities
The architecture-specific entry path must:
- Establish a known stack.
- Clear `.bss`.
- Preserve the incoming `boot_info_t *` passed in `RCX` under the Microsoft x64
  UEFI calling convention.
- Call `kmain(boot_info_t *)`.

### `boot_info_t` Rules
`boot_info_t` is append-only and versioned. New fields go at the end.

Current required fields:
- `version`, `size`, `flags`
- `memory_map`, `memory_map_size`, `memory_desc_size`, `memory_desc_version`
- `modules`, `module_count`, `module_entry_size`
- `rsdp`, `rsdp_length`
- `initfs`, `initfs_size`
- `boot_config`, `boot_config_size`

Current behavior:
- The bootloader fills the structure and the kernel validates the size/version
  before using it.
- Boot modules are still used as the early bootstrap channel, but they are now
  derived from the initfs instead of being loaded one-by-one in the bootloader.

### Initfs Layout
The bootstrap image format is intentionally small and append-only enough for
boot handoff use:
- header with magic `WMINITFS`, version, table sizing, and total image size
- fixed-size entries with type, flags, payload offset/size, and logical path
- raw payload data packed directly behind the table

Current entry types:
- WASMOS app payload
- config payload
- generic data payload

Current bootstrap use:
- bootstrap-marked WASMOS app entries are exposed as `boot_module_t` records
- the first config entry is exposed through `boot_info_t.boot_config`
- the full blob is retained so later code can add a proper initfs reader

## Boot Flow

### High-Level Sequence
1. UEFI loads `BOOTX64.EFI`.
2. The bootloader loads `kernel.elf` and `initfs.img`.
3. The bootloader exits boot services and jumps to kernel entry.
4. The kernel initializes core subsystems and spawns the kernel `init` task.
5. `init` starts `device-manager` from the bootstrap module set exposed by initfs.
6. `device-manager` starts `pci-bus`, consumes inventory records, then starts
   `ata` and `fs-fat`.
7. `init` waits for FAT readiness, then loads `sysinit` from disk through the
   process manager.
8. `sysinit` reads the boot config and starts the configured late user
   processes.
9. The CLI becomes the visible interactive shell.

A minimal COM1-based serial stub keeps the console alive during the steps above.
The AssemblyScript `serial` driver now loads via `device-manager` and invokes
`serial_register()` so console output can switch over from the stub to the new
service as soon as the driver is available.

- `device-manager` merely starts the keyboard WASMOS app alongside the other
  bootstrap drivers; the AssemblyScript driver now polls the PS/2 controller for
  scancodes itself so keyboard presence remains a user-space concern instead of
  spinning kernel knowledge into the microkernel core.

### Practical Boot Ownership
- Bootloader owns UEFI interaction and boot-time file I/O.
- Kernel owns core mechanisms and early bootstrap orchestration.
- `init` owns system bootstrap sequencing once the kernel is alive.
- `sysinit` owns late user process startup policy from boot config only.

This split is intentional: it keeps bootloader policy minimal and prevents
`sysinit` from becoming a second bootstrap coordinator.
