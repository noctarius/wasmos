## Boot Contract

This document describes the full boot handoff chain from UEFI to `kmain`:
the bootloader's responsibilities, the `boot_info_t` contract, the initfs
format, the kernel's two-stage entry sequence, and the memory layout
established before the scheduler starts.

---

### Bootloader Responsibilities

The bootloader is intentionally policy-light. It builds a trustworthy handoff
for the kernel and then gets out of the way. It must not start services, make
scheduling decisions, or embed OS policy.

Concrete responsibilities:
- Open the ESP via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` and read `\kernel.elf`
  and `\initfs.img` into UEFI pool memory.
- Validate the ELF header and load all `PT_LOAD` segments at their stated
  physical addresses (`AllocatePages(EFI_ALLOCATE_ADDRESS, ...)`), including
  segments with misaligned `p_paddr` and zero-BSS (`p_memsz > p_filesz`)
  regions. Segments whose physical address is zero get `EFI_ALLOCATE_ANY_PAGES`.
- Validate the initfs blob against the `WMINITFS` magic, version, header size,
  entry size, and per-entry offset/size bounds before use.
- Locate and snapshot the ACPI RSDP pointer from the UEFI configuration table
  (ACPI 2.0 preferred; ACPI 1.0 as fallback).
- Capture framebuffer parameters before `ExitBootServices()` (see below).
- Snapshot the UEFI memory map and call `ExitBootServices()` with the correct
  `map_key`. Retry if `EFI_INVALID_PARAMETER` is returned (see below).
- Build a single contiguous `boot_info_t` blob and pass its pointer to the
  kernel entry point.
- Transfer control to the kernel ELF entry point (`e_entry`).

---

### UEFI Execution Sequence

Inside `efi_main`:

```
1.  HandleProtocol(image, EFI_LOADED_IMAGE_PROTOCOL_GUID)  → loaded image
2.  HandleProtocol(deviceHandle, EFI_SIMPLE_FILE_SYSTEM)   → volume FS
3.  fs->OpenVolume()                                        → root dir
4.  read_file_alloc(\kernel.elf)                            → kernel_buf
5.  read_file_alloc(\initfs.img)                            → initfs_buf
6.  initfs_valid(initfs_buf)                                → abort if invalid
7.  elf_is_valid(kernel_buf)                                → abort if invalid
8.  load PT_LOAD segments into physical addresses
9.  find_acpi_rsdp(system)                                  → rsdp, rsdp_length
10. capture_framebuffer_snapshot(system)                    → framebuffer_snapshot
11. ExitBootServices() retry loop (below)
12. fill boot_info_t blob
13. jump to ehdr->e_entry(boot_info_t *)
```

Step 10 (framebuffer capture) happens before the `ExitBootServices()` loop
because GOP calls (`LocateProtocol`, `ConnectController`) can invalidate
`map_key` by mutating the UEFI memory map. Capturing the snapshot early keeps
the final `map_key` stable.

---

### Framebuffer Discovery

The bootloader uses a three-stage fallback strategy:

**Stage 1 — `LocateProtocol(GOP_GUID)`.**
The direct global GOP lookup. Succeeds on most OVMF/QEMU configurations.

**Stage 2 — `HandleProtocol(ConsoleOutHandle, GOP_GUID)` + `LocateHandleBuffer`.**
If `LocateProtocol` fails, try the console output handle directly. If that also
fails, enumerate all handles with GOP via `LocateHandleBuffer`, calling
`ConnectController` on each candidate before `HandleProtocol`.

**Stage 3 — PCI direct scan fallback.**
If all UEFI GOP paths fail, scan PCI config space (bus 0–255, device 0–31,
function 0–7) for class-code `0x03` (display controller) and probe each MMIO
BAR (32-bit and 64-bit types) by writing `0xFFFFFFFF` and reading back the mask
to determine size. The largest candidate BAR is selected. Width, height, and
stride default to 1024×768×1024 when this path is taken.

Before the Stage 1 attempt, the bootloader calls `connect_graphics_controllers`
to connect the console output handle, standard error handle, and all
`EFI_GRAPHICS_OUTPUT_PROTOCOL`, `EFI_PCI_IO_PROTOCOL`, and
`EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` handles. This is necessary on OVMF builds
where GOP drivers are not automatically connected.

**Pixel format encoding in `boot_info_t.flags`:**
Bits 11:8 carry the `EFI_GRAPHICS_PIXEL_FORMAT` value from the GOP mode info:

| Value | Meaning                          |
|-------|----------------------------------|
| 0     | RGB (8bpp per channel, reserved) |
| 1     | BGR (8bpp per channel, reserved) |
| 2     | PixelBitMask                     |
| 3     | BltOnly (no linear framebuffer)  |

The pixel format is packed as `(pixel_format & 0xF) << BOOT_INFO_FLAG_GOP_PIXEL_FORMAT_SHIFT`
(shift = 8). The PCI fallback path always writes `BOOT_INFO_FLAG_GOP_PRESENT`
with format bits zero (treated as RGB by convention).

---

### ACPI RSDP Discovery

The bootloader scans `EFI_SYSTEM_TABLE.ConfigurationTable` for two GUIDs:
- ACPI 2.0: `{8868e871-e4f1-11d3-bc22-0080c73c8881}`
- ACPI 1.0: `{eb9d2d30-2d88-11d3-9a16-00902773fc14d}`

ACPI 2.0 is preferred. On the first ACPI 2.0 match the search stops. ACPI 1.0
is accepted as a fallback if no 2.0 entry is found.

`rsdp_length` is set to 20 bytes for revision 0 (ACPI 1.0) or to
`acpi_rsdp_t.length` for revision ≥ 2 (ACPI 2.0 extended RSDP).

---

### ExitBootServices Retry Loop

UEFI requires that `ExitBootServices` is called with the `map_key` returned by
the most recent `GetMemoryMap` call. Any UEFI allocation between `GetMemoryMap`
and `ExitBootServices` — including pool allocations and `ConnectController`
calls — can increment the firmware's internal generation counter and invalidate
the key.

The bootloader handles this with a retry loop:

```
loop:
  GetMemoryMap(&mmap_size, mmap_buf, &map_key, ...)
    → if EFI_BUFFER_TOO_SMALL: grow mmap_buf and retry
  fill boot_info_t (no UEFI allocations in this window)
  ExitBootServices(image, map_key)
    → if EFI_SUCCESS: done
    → if EFI_INVALID_PARAMETER: map_key stale, go back to loop
    → any other error: fatal
```

The framebuffer snapshot is captured before this loop so no GOP calls occur
inside it. The `boot_info_t` blob is allocated before the loop on the first
pass; if the loop retries, the existing allocation is reused (no
`AllocatePages` inside the retry window).

---

### Boot Blob Memory Layout

All bootloader-provided data for the kernel lives in a single contiguous
allocation made with `AllocatePages(EFI_ALLOCATE_ANY_PAGES, ...)`:

```
Offset 0:                   boot_info_t header           (sizeof(boot_info_t))
Offset sizeof(boot_info_t): UEFI memory map copy         (mmap_size bytes)
After memory map:           initfs.img copy              (initfs_size bytes)
After initfs copy:          boot_module_t table          (module_count × sizeof(boot_module_t))
```

`boot_info_t` pointers are set to offsets within this same allocation:
- `boot_info->memory_map` → start of the memory map copy
- `boot_info->initfs` → start of the initfs copy
- `boot_info->boot_config` → pointer into the initfs copy at the offset of the
  first `WASMOS_INITFS_ENTRY_CONFIG` entry's payload
- `boot_info->modules` → start of the boot_module_t table

All pointers are physical addresses valid immediately after `ExitBootServices`.
The kernel must not use them after establishing virtual memory unless it builds
higher-half aliases first (see Kernel Bootstrap section below).

---

### `boot_info_t` Contract

Current version: `BOOT_INFO_VERSION = 4`.

| Field                             | Type       | Meaning                                          |
|-----------------------------------|------------|--------------------------------------------------|
| `version`                         | `uint32_t` | `BOOT_INFO_VERSION`; kernel rejects mismatches   |
| `size`                            | `uint32_t` | `sizeof(boot_info_t)`; used for version checking |
| `flags`                           | `uint32_t` | see flag bits below                              |
| `memory_map`                      | `void *`   | pointer to UEFI memory descriptor array          |
| `memory_map_size`                 | `uint64_t` | total bytes in the map array                     |
| `memory_desc_size`                | `uint64_t` | size of one `EFI_MEMORY_DESCRIPTOR`              |
| `memory_desc_version`             | `uint32_t` | descriptor struct version from UEFI              |
| `framebuffer_base`                | `void *`   | linear framebuffer physical base address         |
| `framebuffer_size`                | `uint64_t` | framebuffer byte length                          |
| `framebuffer_width`               | `uint32_t` | horizontal resolution in pixels                  |
| `framebuffer_height`              | `uint32_t` | vertical resolution in pixels                    |
| `framebuffer_pixels_per_scanline` | `uint32_t` | scanline stride in pixels                        |
| `modules`                         | `void *`   | pointer to `boot_module_t` array                 |
| `module_count`                    | `uint32_t` | number of entries in the module array            |
| `module_entry_size`               | `uint32_t` | `sizeof(boot_module_t)` for forward compat       |
| `rsdp`                            | `void *`   | ACPI RSDP physical pointer (null if not found)   |
| `rsdp_length`                     | `uint32_t` | 20 for ACPI 1.0; full length for ACPI 2.0        |
| `initfs`                          | `void *`   | pointer to initfs blob copy                      |
| `initfs_size`                     | `uint32_t` | initfs blob byte length                          |
| `boot_config`                     | `void *`   | pointer to boot config payload (within initfs)   |
| `boot_config_size`                | `uint32_t` | boot config payload byte length                  |

**Flag bits:**

| Bit / field                              | Meaning                                  |
|------------------------------------------|------------------------------------------|
| `BOOT_INFO_FLAG_GOP_PRESENT` (bit 0)     | framebuffer fields are valid             |
| `BOOT_INFO_FLAG_MODULES_PRESENT` (bit 1) | modules array is valid                   |
| `BOOT_INFO_FLAG_INITFS_PRESENT` (bit 2)  | initfs pointer and size are valid        |
| bits 11:8                                | `EFI_GRAPHICS_PIXEL_FORMAT` (4 bits)     |

**Rules:**
- `boot_info_t` is append-only and versioned. New fields go at the end.
- The kernel validates `version` and `size` before accessing any field.
- Callers must check individual flag bits before dereferencing the corresponding
  pointer fields.

---

### `boot_module_t` Contract

Each WASMOS-APP entry in the initfs generates one `boot_module_t` record:

| Field      | Type        | Meaning                                               |
|------------|-------------|-------------------------------------------------------|
| `base`     | `uint64_t`  | physical address of the payload (within initfs copy)  |
| `size`     | `uint32_t`  | payload byte length                                   |
| `type`     | `uint32_t`  | `BOOT_MODULE_TYPE_WASMOS_APP = 1`                     |
| `reserved` | `uint32_t`  | zero                                                  |
| `name`     | `char[48]`  | NUL-terminated copy of `wasmos_initfs_entry_t.path`   |

Modules are ordered by their position in the initfs entry table.
Module indices in the kernel match initfs entry position (first WASMOS-APP
entry = module 0, etc.).

---

### Initfs Format

The bootstrap image format is minimal and append-only.

**Header (`wasmos_initfs_header_t`, packed):**

| Field         | Type       | Value / meaning                          |
|---------------|------------|------------------------------------------|
| `magic`       | `char[8]`  | `"WMINITFS"` (no null terminator)        |
| `version`     | `uint16_t` | `WASMOS_INITFS_VERSION = 1`              |
| `header_size` | `uint16_t` | `sizeof(wasmos_initfs_header_t)` = 24    |
| `entry_count` | `uint32_t` | number of entries                        |
| `entry_size`  | `uint32_t` | `sizeof(wasmos_initfs_entry_t)` = 112    |
| `total_size`  | `uint32_t` | total blob size in bytes                 |
| `reserved`    | `uint32_t` | zero                                     |

**Entry table (`wasmos_initfs_entry_t`, packed, one per entry):**

| Field    | Type       | Meaning                                                     |
|----------|------------|-------------------------------------------------------------|
| `type`   | `uint32_t` | see entry types below                                       |
| `flags`  | `uint32_t` | `WASMOS_INITFS_ENTRY_FLAG_BOOTSTRAP = (1 << 0)`             |
| `offset` | `uint32_t` | byte offset of payload from start of blob                   |
| `size`   | `uint32_t` | payload byte length                                         |
| `path`   | `char[96]` | NUL-terminated logical path (e.g. `system/drivers/ata.wap`) |

Entry table immediately follows the header. Payload data is packed after the
entry table.

**Entry types:**

| Value | Name                             | Meaning                    |
|-------|----------------------------------|----------------------------|
| 0     | `WASMOS_INITFS_ENTRY_NONE`       | unused/padding             |
| 1     | `WASMOS_INITFS_ENTRY_WASMOS_APP` | WASMOS-APP package payload |
| 2     | `WASMOS_INITFS_ENTRY_CONFIG`     | boot config binary blob    |
| 3     | `WASMOS_INITFS_ENTRY_DATA`       | generic data payload       |

**Bootloader validation checks** (fail-fast, abort if any fails):
1. blob size ≥ `sizeof(wasmos_initfs_header_t)`
2. `magic == "WMINITFS"`
3. `version == WASMOS_INITFS_VERSION`
4. `header_size == sizeof(wasmos_initfs_header_t)`
5. `entry_size == sizeof(wasmos_initfs_entry_t)`
6. `total_size ≤ loaded file size`
7. for each entry: `entry->offset + entry->size ≤ total_size`
8. for each entry: `entry->offset ≥ header_size + entry_count * entry_size`
   (payload data cannot overlap the header or entry table)

---

### Kernel Memory Layout

The kernel ELF has two load groups with different physical and virtual addresses.

**Group 1 — bootstrap (loaded at low physical addresses):**

```
VMA == LMA == 0x100000  (KERNEL_LOAD_BASE)
  .bootstrap.text  (.start.low)       — _start and page-table fill routines
  .bootstrap.data  (.start.low.data)  — bootstrap_boot_info pointer slot
  .bootstrap.bss   (.start.low.bss)   — bootstrap page tables (8 × 4 KiB)
```

Bootstrap sections occupy from `0x100000` to `__bootstrap_end`, rounded up to
the next 2 MiB boundary for `__kernel_lma`.

**Group 2 — kernel proper (higher-half virtual, low physical):**

```
VMA == 0xFFFFFFFF80000000 + __kernel_lma  (KERNEL_HIGHER_HALF_BASE + __kernel_lma)
LMA == __kernel_lma

  .text     — kernel code
  .rodata   — read-only data (4 KiB aligned)
  .data     — initialized data (4 KiB aligned)
  .bss      — zero-initialized data (4 KiB aligned)
  __stack_top  — 64 KiB stack area above .bss
```

The invariant is `vaddr - KERNEL_HIGHER_HALF_BASE == lma` for all higher-half
sections. This means the bootstrap large-page mapping at
`PML4[511][510][0..31]` (64 MiB, 2 MiB pages starting at physical 0) resolves
the same bytes as the ELF `PT_LOAD` population at `p_paddr == lma`.

---

### Kernel Entry and Bootstrap

`BOOTX64.EFI` calls `kernel_entry(boot_info)` via the Microsoft x64 UEFI
calling convention, which places the first argument in `RCX`. The ELF entry
point is `_start` in `.start.low`.

**Stage 1: `_start` (physical address ~0x100000, pre-paging)**

1. Save `RCX` (`boot_info_t *`) into `bootstrap_boot_info` (a `.start.low.data`
   slot accessible by RIP-relative addressing).
2. Zero-fill the three page-table root pages:
   `bootstrap_pml4`, `bootstrap_pdpt_low`, `bootstrap_pdpt_high`.
3. Fill four 2 MiB-page PDs (`bootstrap_pd0`–`pd3`) for the identity map
   covering physical 0–4 GiB (low slot, PML4[0]).
4. Fill one 32-entry 2 MiB-page PD (`bootstrap_pd_high`) covering physical
   0–64 MiB (higher-half slot, PML4[511][510]).
5. Wire up PML4 → PDPT → PD for both slots.
6. Load `bootstrap_pml4` into `CR3`.
7. Jump to `_start_high` via `movabs` + indirect jump (required because the
   target VMA is above 4 GiB).

**Stage 2: `_start_high` (VMA 0xFFFFFFFF80000000+, paging active)**

1. Set `RSP` to `__stack_top` (higher-half virtual address).
2. Zero the `.bss` region (`__bss_start` to `__bss_end`).
3. Load `boot_info_t *` from `bootstrap_boot_info` using `movabs` (physical
   address accessed via the still-present identity map).
4. Place the pointer in `RDI` (System V x86_64 first argument register).
5. Call `kmain(boot_info_t *)`.

`kmain` is responsible for building the permanent higher-half kernel page tables
and may remove the low identity-map slot once the permanent tables are active.

**Bootstrap page table layout:**

```
PML4[0]   → bootstrap_pdpt_low
  [0..3]  → bootstrap_pd0..pd3  (identity 0–4 GiB, 2 MiB pages, P+W)

PML4[511] → bootstrap_pdpt_high
  [510]   → bootstrap_pd_high   (maps physical 0–64 MiB to VMA 0xFFFFFFFF80000000)
```

The 64 MiB higher-half window covers the kernel `.text`, `.rodata`, `.data`,
`.bss`, and initial stack regardless of how large the kernel binary grows up to
that limit.

---

### Boot Flow

1. UEFI loads `BOOTX64.EFI`.
2. Bootloader loads `kernel.elf` and `initfs.img` from the ESP root.
3. Bootloader validates the ELF header, loads all `PT_LOAD` segments, validates
   the initfs, locates ACPI RSDP, captures the framebuffer snapshot.
4. Bootloader exits boot services and fills `boot_info_t`.
5. `_start` builds bootstrap page tables and jumps to `_start_high`.
6. `_start_high` clears `.bss` and calls `kmain(boot_info_t *)`.
7. `kmain` initializes core subsystems and spawns the kernel `init` task.
8. `init` starts `device-manager` from the bootstrap module set.
9. `device-manager` starts `pci-bus`, consumes inventory, starts `ata` and
   `fs-fat` via rules.
10. `init` waits for FAT readiness, then loads `sysinit` from disk.
11. `sysinit` reads the boot config and starts configured late user processes.

A COM1 serial stub (`0x3F8`) is active from early in `kmain` through all of the
above, providing diagnostic output before any driver is online.

---

### Boot Ownership

| Component     | Owns                                                             |
|---------------|------------------------------------------------------------------|
| `BOOTX64.EFI` | UEFI interaction, file I/O, memory map, blob construction        |
| `_start`      | bootstrap page tables, CR3 load                                  |
| `_start_high` | BSS clear, stack setup, `kmain` call                             |
| `kmain`       | core subsystem init, permanent page tables, `init` task spawn    |
| `init`        | system bootstrap sequencing (`device-manager` → FAT → `sysinit`) |
| `sysinit`     | late user process startup from boot config only                  |

`sysinit` is intentionally narrow. It reads the `sysinit.spawn` list from the
boot config and starts those processes. It is not a second bootstrap coordinator
and does not make device or mount policy decisions.

---

### Invariants

1. **The bootloader sets no OS policy.** It loads files, fills `boot_info_t`,
   and transfers control. No service startup, no scheduling decisions.
2. **`boot_info_t` is validated before use.** The kernel checks `version` and
   `size` before accessing any field. Flag bits are checked before
   dereferencing pointer fields.
3. **`boot_info_t` is append-only.** New fields go at the end. Fields are never
   removed or reordered. The `size` field allows future callers to detect older
   structs.
4. **Initfs is validated before use.** The bootloader aborts on any validation
   failure rather than passing a potentially corrupt blob to the kernel.
5. **Bootstrap page tables cover both identity and higher-half windows.**
   The identity map (0–4 GiB) keeps `_start` execution coherent during the
   transition; the higher-half window (64 MiB at `0xFFFFFFFF80000000`) is the
   permanent kernel virtual address range.
6. **No UEFI calls between the final `GetMemoryMap` and `ExitBootServices`.**
   The bootloader builds the `boot_info_t` blob (no allocation) in this window
   to keep `map_key` stable.
