## Drivers and Services

This document describes each implemented driver and service: its role, the IPC
endpoints it exposes, the capabilities it requires, and the component-level
interactions that make up the boot chain. The authoritative opcode table for
all IPC message types is in `src/drivers/include/wasmos_driver_abi.h`.

---

### IPC Opcode Allocation

All message types are defined in `wasmos_driver_abi.h`. Ranges are reserved by
subsystem; opcodes within a range are not scattered into other ranges.

| Range         | Subsystem                                          |
|---------------|----------------------------------------------------|
| `0x100–0x1FF` | chardev                                            |
| `0x200–0x2FF` | process-manager (spawn, wait, kill, registry, DMA) |
| `0x300–0x3FF` | block device                                       |
| `0x400–0x4FF` | filesystem / fs-manager                            |
| `0x600–0x6FF` | framebuffer text (fbtext)                          |
| `0x700–0x7FF` | virtual terminal (VT)                              |
| `0x800–0x8FF` | input (keyboard, mouse, RTC, virtio-serial)        |
| `0x900–0x9FF` | device-manager                                     |

---

### Implemented Drivers

#### `ata` — PIO ATA Block Driver

- **Kind:** WASM driver; `FLAG_DRIVER|FLAG_STORAGE_BOOTSTRAP`
- **PCI match:** class `0x01`, subclass `0x01`, any vendor/device
- **Capabilities:** `io.port` (0x01F0–0x03F7), `irq.route`, `dma.buffer`
- **Registers:** `block` service endpoint
- **Entry bindings:** `proc.endpoint`
- **Resources:** 16 stack pages, 16 heap pages

Supports ATA IDENTIFY and PIO read. Publishes block-device records to
device-manager via `DEVMGR_PUBLISH_BLOCK_DEVICE` once a device is confirmed
present. The `storage_bootstrap=true` flag pins this driver to the initfs rule
set; it cannot be overridden by runtime boot-FAT rules.

Block IPC opcodes:

| Opcode                    | Value   | Meaning           |
|---------------------------|---------|-------------------|
| `BLOCK_IPC_READ_REQ`      | `0x300` | Read sectors      |
| `BLOCK_IPC_WRITE_REQ`     | `0x301` | Write sectors     |
| `BLOCK_IPC_IDENTIFY_REQ`  | `0x302` | Identify device   |
| `BLOCK_IPC_READ_RESP`     | `0x380` | Read response     |
| `BLOCK_IPC_WRITE_RESP`    | `0x381` | Write response    |
| `BLOCK_IPC_IDENTIFY_RESP` | `0x382` | Identify response |
| `BLOCK_IPC_ERROR`         | `0x3FF` | Error response    |

#### `fs-fat` — FAT12/16/32 Filesystem Driver

- **Kind:** WASM driver
- **Capabilities:** `ipc.basic`
- **Requires:** `block` service endpoint (looked up via PM registry)
- **Entry bindings:** `proc.endpoint`
- **Resources:** 16 stack pages, 16 heap pages

Supports FAT12/16/32 on the active ATA block device. Exposes root and
subdirectory listing, file open/read/stat/seek/close, PM app loading via
`FS_IPC_READ_APP_REQ`, and the full read-only file API used by libc. Follows
FAT12/16/32 cluster chains for multi-cluster reads.

`fs-fat` does not directly register a named endpoint. Instead it hands its
endpoint to `fs-manager`, which registers the unified `fs.vfs` endpoint on its
behalf.

Filesystem IPC opcodes:

| Opcode                 | Value   | Meaning                     |
|------------------------|---------|-----------------------------|
| `FS_IPC_OPEN_REQ`      | `0x400` | Open file                   |
| `FS_IPC_READ_REQ`      | `0x401` | Read bytes                  |
| `FS_IPC_CLOSE_REQ`     | `0x402` | Close file                  |
| `FS_IPC_STAT_REQ`      | `0x403` | Stat file                   |
| `FS_IPC_READY_REQ`     | `0x404` | Check readiness             |
| `FS_IPC_SEEK_REQ`      | `0x405` | Seek                        |
| `FS_IPC_WRITE_REQ`     | `0x406` | Write bytes                 |
| `FS_IPC_UNLINK_REQ`    | `0x407` | Delete file                 |
| `FS_IPC_MKDIR_REQ`     | `0x408` | Create directory            |
| `FS_IPC_RMDIR_REQ`     | `0x409` | Remove directory            |
| `FS_IPC_READDIR_REQ`   | `0x410` | Read directory entries      |
| `FS_IPC_CHDIR_REQ`     | `0x412` | Change directory            |
| `FS_IPC_READ_APP_REQ`  | `0x413` | Read WASMOS-APP blob for PM |
| `FS_IPC_READ_PATH_REQ` | `0x414` | Read file by path           |
| `FS_IPC_RESP`          | `0x480` | Success response            |
| `FS_IPC_STREAM`        | `0x481` | Streaming data              |
| `FS_IPC_ERROR`         | `0x4FF` | Error response              |

#### `fs-init` — Initfs Listing Driver

- **Kind:** WASM driver
- **Registers:** `initfs.rules` service endpoint
- **Entry bindings:** `proc.endpoint`

Exposes the initfs image (loaded into RAM at boot) as a read-only filesystem.
Used by device-manager to load the bootstrap rule file before the FAT
partition is online.

#### `chardev` — Console Character Device

- **Kind:** WASM driver
- **Entry bindings:** `proc.endpoint`

IPC-backed character device server providing console read and write. Connects
to the kernel's `console_ring_t` (4080-byte ring buffer).

Chardev IPC opcodes:

| Opcode                        | Value   | Meaning          |
|-------------------------------|---------|------------------|
| `WASM_CHARDEV_IPC_READ_REQ`   | `0x100` | Read characters  |
| `WASM_CHARDEV_IPC_WRITE_REQ`  | `0x101` | Write characters |
| `WASM_CHARDEV_IPC_READ_RESP`  | `0x180` | Read response    |
| `WASM_CHARDEV_IPC_WRITE_RESP` | `0x181` | Write response   |
| `WASM_CHARDEV_IPC_ERROR_RESP` | `0x1FF` | Error response   |

#### `framebuffer` — EFI/VESA Framebuffer Driver (Native)

- **Kind:** native ELF driver; `FLAG_DRIVER|FLAG_NATIVE`
- **Capabilities:** `dma.buffer`

Probes the EFI framebuffer address provided via `boot_info_t`. Validates
geometry and maps framebuffer pages into the driver's device virtual region
through the native-driver API. Serves the fbtext IPC protocol for cell writes,
cursor, scrolling, clearing, mode queries, and console-ring passthrough.

Framebuffer text IPC opcodes:

| Opcode                          | Value   | Meaning                           |
|---------------------------------|---------|-----------------------------------|
| `FBTEXT_IPC_CELL_WRITE_REQ`     | `0x600` | Write character cell              |
| `FBTEXT_IPC_CURSOR_SET_REQ`     | `0x601` | Set cursor position               |
| `FBTEXT_IPC_SCROLL_REQ`         | `0x602` | Scroll region                     |
| `FBTEXT_IPC_CLEAR_REQ`          | `0x603` | Clear screen                      |
| `FBTEXT_IPC_CONSOLE_MODE_REQ`   | `0x604` | Enable/disable console ring drain |
| `FBTEXT_IPC_GEOMETRY_REQ`       | `0x605` | Query cols/rows                   |
| `FBTEXT_IPC_GFX_OVERLAY_REQ`    | `0x606` | Lock/unlock for compositor        |
| `FBTEXT_IPC_QUERY_CAPS_REQ`     | `0x607` | Query capability bitmask          |
| `FBTEXT_IPC_QUERY_MODES_REQ`    | `0x608` | Query available video modes       |
| `FBTEXT_IPC_SET_RESOLUTION_REQ` | `0x609` | Request resolution change         |
| `FBTEXT_IPC_RESP`               | `0x680` | Success response                  |
| `FBTEXT_IPC_ERROR`              | `0x6FF` | Error response                    |

Capability flags: `FBTEXT_CAP_SET_RESOLUTION = 1<<0`, `FBTEXT_CAP_QUERY_MODES = 1<<1`.

#### `framebuffer_pci` — PCI VGA Framebuffer Driver (Native)

- **Kind:** native ELF driver; `FLAG_DRIVER|FLAG_NATIVE`
- **PCI match:** class `0x03`, subclass `0x00`, prog_if `0x00`
- **Capabilities:** `dma.buffer`

PCI VGA variant of the framebuffer driver. Spawned by the boot-FAT rule when a
matching PCI graphics device is discovered.

#### `keyboard` — PS/2 Keyboard Driver

- **Kind:** WASM driver
- **ACPI match:** class `0x09`, subclass `0x00` (PNP0303 — AT keyboard/i8042)
- **Capabilities:** `io.port`, `irq.route`
- **Entry bindings:** `proc.endpoint`

Services subscriber registration and delivers key events.

| Opcode                   | Value   | Meaning                 |
|--------------------------|---------|-------------------------|
| `KBD_IPC_SUBSCRIBE_REQ`  | `0x800` | Register for key events |
| `KBD_IPC_SUBSCRIBE_RESP` | `0x880` | Subscription confirmed  |
| `KBD_IPC_KEY_NOTIFY`     | `0x801` | Key event notification  |

#### `mouse` — PS/2 Mouse Driver

- **Kind:** WASM driver
- **ACPI match:** class `0x09`, subclass `0x02` (PNP0F03/PNP0F13 — PS/2 mouse)
- **Capabilities:** `io.port`, `irq.route`
- **Entry bindings:** `proc.endpoint`

| Opcode                     | Value   | Meaning                                            |
|----------------------------|---------|----------------------------------------------------|
| `MOUSE_IPC_SUBSCRIBE_REQ`  | `0x810` | Register for move events                           |
| `MOUSE_IPC_SUBSCRIBE_RESP` | `0x890` | Subscription confirmed                             |
| `MOUSE_IPC_MOVE_NOTIFY`    | `0x811` | Move/button event (arg0=dx, arg1=dy, arg2=buttons) |

#### `serial` — UART Serial Driver

- **Kind:** WASM driver
- **ACPI match:** class `0x07`, subclass `0x00` (PNP0501 — 16550A serial)
- **Capabilities:** `io.port`
- **Entry bindings:** `proc.endpoint`

#### `rtc` — Real-Time Clock Driver

- **Kind:** WASM driver
- **ACPI match:** class `0x08`, subclass `0x03` (PNP0B00 — CMOS RTC)
- **Capabilities:** `io.port`, `irq.route`
- **Entry bindings:** `proc.endpoint`

| Opcode              | Value   | Meaning           |
|---------------------|---------|-------------------|
| `RTC_IPC_READ_REQ`  | `0x820` | Read current time |
| `RTC_IPC_SET_REQ`   | `0x821` | Set time          |
| `RTC_IPC_READ_RESP` | `0x8A0` | Read response     |
| `RTC_IPC_SET_RESP`  | `0x8A1` | Set response      |
| `RTC_IPC_ERROR`     | `0x8FF` | Error response    |

#### `virtio-serial` — VirtIO Serial Driver

- **Kind:** WASM driver
- **PCI match:** class `0x07`, subclass `0x00`, prog_if `0x00`, vendor `0x1AF4` (VirtIO)
- **Capabilities:** `io.port`
- **Registers:** `virtio.serial` service endpoint
- **Entry bindings:** `proc.endpoint`

Used for virtual input testing from the host. Exposes register read/write over
IPC.

| Opcode                              | Value   | Meaning               |
|-------------------------------------|---------|-----------------------|
| `VIRTIO_SERIAL_IPC_QUERY_REQ`       | `0x830` | Query device presence |
| `VIRTIO_SERIAL_IPC_READ_REG32_REQ`  | `0x831` | Read 32-bit register  |
| `VIRTIO_SERIAL_IPC_WRITE_REG32_REQ` | `0x832` | Write 32-bit register |
| `VIRTIO_SERIAL_IPC_RESP`            | `0x8B0` | Response              |
| `VIRTIO_SERIAL_IPC_ERROR`           | `0x8BF` | Error                 |

---

### Implemented Services

#### `process-manager` — Process and Service Registry

Kernel-hosted service that owns process lifecycle, service registry, and DMA
buffer coordination. It is not a WASMOS-APP but runs in the same IPC space.

Process-manager IPC opcodes (selected):

| Opcode                          | Value   | Meaning                                  |
|---------------------------------|---------|------------------------------------------|
| `PROC_IPC_SPAWN`                | `0x200` | Spawn by module index (async)            |
| `PROC_IPC_WAIT`                 | `0x201` | Wait for process exit                    |
| `PROC_IPC_KILL`                 | `0x202` | Kill process                             |
| `PROC_IPC_STATUS`               | `0x203` | Query process status                     |
| `PROC_IPC_SPAWN_NAME`           | `0x204` | Spawn by name                            |
| `PROC_IPC_SPAWN_CAPS`           | `0x205` | Spawn with capability descriptor         |
| `PROC_IPC_MODULE_META`          | `0x206` | Query module metadata by index           |
| `PROC_IPC_MODULE_META_PATH`     | `0x207` | Query module metadata by path            |
| `PROC_IPC_SPAWN_CAPS_V2`        | `0x208` | Spawn with extended caps (DMA windows)   |
| `PROC_IPC_SPAWN_PATH`           | `0x209` | Spawn from explicit file path            |
| `PROC_IPC_SPAWN_PATH_CAPS`      | `0x20A` | Spawn from path with I/O+IRQ caps        |
| `PROC_IPC_SPAWN_SYNC`           | `0x20B` | Spawn by index, block until NOTIFY_READY |
| `PROC_IPC_NOTIFY_READY`         | `0x20C` | Service signals initialization complete  |
| `PROC_IPC_SPAWN_CAPS_SYNC`      | `0x20D` | Spawn caps, block until ready            |
| `PROC_IPC_SPAWN_PATH_SYNC`      | `0x20E` | Spawn path, block until ready            |
| `PROC_IPC_SPAWN_PATH_CAPS_SYNC` | `0x20F` | Spawn path+caps, block until ready       |
| `PROC_IPC_RESP`                 | `0x280` | Generic success response                 |
| `PROC_IPC_ERROR`                | `0x2FF` | Generic error response                   |
| `SVC_IPC_REGISTER_REQ`          | `0x220` | Register named service endpoint          |
| `SVC_IPC_LOOKUP_REQ`            | `0x221` | Look up named service endpoint           |
| `SVC_IPC_REGISTER_RESP`         | `0x2A0` | Register response                        |
| `SVC_IPC_LOOKUP_RESP`           | `0x2A1` | Lookup response                          |
| `PROC_IPC_DMA_MAP_BORROW_REQ`   | `0x230` | Map DMA borrow buffer                    |
| `PROC_IPC_DMA_SYNC_BORROW_REQ`  | `0x231` | Sync DMA borrow buffer                   |
| `PROC_IPC_DMA_UNMAP_BORROW_REQ` | `0x232` | Unmap DMA borrow buffer                  |
| `PROC_IPC_DMA_BORROW_RESP`      | `0x2B0` | DMA borrow success response              |
| `PROC_IPC_DMA_BORROW_ERROR`     | `0x2BF` | DMA borrow error response                |

The `PROC_SPAWN_PATH_FLAG_DETACH` flag on `PROC_IPC_SPAWN_PATH` skips the
ready-wait even for service/driver kinds.

#### `device-manager` — Hardware Discovery and Driver Lifecycle

- **Kind:** WASM service
- **Capabilities:** `ipc.basic`
- **Requires:** `proc` endpoint
- **Registers:** `devmgr.inv` (inventory endpoint), `devmgr.query` (query endpoint)
- **Entry bindings:** `proc.endpoint`, `module.count`
- **Resources:** 64 stack pages, 64 heap pages

Coordinates early hardware startup in user space. Spawned first by the kernel
`init` path. Manages four event loops:

| Event loop            | Endpoint variable          | Purpose                 |
|-----------------------|----------------------------|-------------------------|
| `g_dm_ipc_loop`       | `g_dm.reply_endpoint`      | Main request/reply      |
| `g_dm_inventory_loop` | `g_dm.inventory_endpoint`  | PCI/ACPI device records |
| `g_dm_query_loop`     | `g_dm.query_endpoint`      | Mount and block queries |
| `g_dm_rules_loop`     | `g_dm.rule_reply_endpoint` | Rule file FS replies    |

**Phase state machine:**

```
HW_PHASE_INIT → HW_PHASE_SPAWN ⇄ HW_PHASE_IDLE → HW_PHASE_FAILED
```

In `HW_PHASE_SPAWN`, `next_spawn_target()` returns the next pending target
from this priority order: `HW_SPAWN_PCI_BUS`, `HW_SPAWN_ACPI_BUS`,
`HW_SPAWN_RULE_PATH`, `HW_SPAWN_FAT`. When no targets remain, transitions to
`HW_PHASE_IDLE`. Failures transition to `HW_PHASE_FAILED`.

**Spawn retry:** rule-path spawns retry up to 8 times; `fs-fat` spawns retry
up to 8 times; on exhaustion the spawn is abandoned.

**NOTIFY_READY condition:** sent exactly once when `boot_mount_ready == 1` AND
`rules_boot_loaded == 1`.

**In-memory device registries:**

```c
pci_device_record_t   registry[DEVICE_REGISTRY_CAP=64];
block_device_record_t block_registry[BLOCK_REGISTRY_CAP=16];
```

The `pci_device_record_t` carries: bus/device/function, class/subclass/prog_if,
vendor_id/device_id, io_port_base, mmio_hint, irq_hint.

The `block_device_record_t` carries: unit (0–255), sector_count, canonical_id
(`block:<transport>:<addr>:<unit>`, 64 bytes), hash_id (16-char hex prefix of
SHA-256 of canonical_id).

Device-manager IPC opcodes:

| Opcode                         | Value   | Meaning                                 |
|--------------------------------|---------|-----------------------------------------|
| `DEVMGR_PUBLISH_DEVICE`        | `0x900` | PCI/ACPI device record from bus service |
| `DEVMGR_PCI_SCAN_DONE`         | `0x901` | pci-bus signals scan complete           |
| `DEVMGR_QUERY_MOUNT_REQ`       | `0x902` | Query mount path for block device       |
| `DEVMGR_PUBLISH_BLOCK_DEVICE`  | `0x903` | Block device registered by ata driver   |
| `DEVMGR_QUERY_BLOCK_MOUNT_REQ` | `0x904` | Query block device mount by unit        |
| `DEVMGR_ACPI_SCAN_DONE`        | `0x905` | acpi-bus signals scan complete          |
| `DEVMGR_MOUNT_INFO`            | `0x980` | Mount info response                     |
| `DEVMGR_QUERY_DONE`            | `0x981` | Query done response                     |
| `DEVMGR_BLOCK_MOUNT_INFO`      | `0x982` | Block mount info response               |

`DEVMGR_PUBLISH_DEVICE` with `bus=0xFF` in `arg0[31:24]` marks a non-PCI ISA
device published by `acpi-bus`; `arg2` carries the I/O base address.

#### `pci-bus` — PCI Config-Space Enumerator

- **Kind:** WASM service
- **Capabilities:** `io.port` (0x0CF8–0x0CFF)
- **Entry bindings:** (receives `proc_endpoint` as first arg)

Performs a full PCI configuration space scan using the standard mechanism-1
I/O ports:
- `PCI_CFG_ADDR_PORT = 0x0CF8` — address register
- `PCI_CFG_DATA_PORT = 0x0CFC` — data register

Scan scope: 256 buses × 32 devices × 8 functions. For each valid device
(vendor_id ≠ 0xFFFF), reads class/subclass/prog_if, vendor/device ID, BAR0
(MMIO/I/O hint), and IRQ line. Publishes each record via
`DEVMGR_PUBLISH_DEVICE` to the `devmgr.inv` endpoint with fields packed
into four `uint32_t` args:

```
arg0 = (bus<<24) | (device<<16) | (function<<8) | class_code
arg1 = (subclass<<24) | (prog_if<<16) | vendor_id
arg2 = (io_port_base<<16) | device_id
arg3 = (io_port_base<<16) | (irq_hint<<8) | mmio_hint
```

Sends `DEVMGR_PCI_SCAN_DONE` after the last record, then calls
`PROC_IPC_NOTIFY_READY`.

#### `acpi-bus` — ACPI Table Parser and ISA Device Publisher

- **Kind:** WASM service
- **Capabilities:** `mmio.map`
- **Entry bindings:** (receives `proc_endpoint` as first arg)

Obtains the RSDP via `wasmos_acpi_rsdp_info()`. If RSDP revision ≥ 2 and
`xsdt_address ≠ 0`, follows the XSDT path; otherwise RSDT. Locates the DSDT
through the FACP (Fixed ACPI Description Table) entry in the SDT.

Scans the DSDT AML byte stream for 16-byte-aligned `0x41D0` PNP vendor bytes
(all ISA/PNP devices use vendor `0x41D0`). For each PNP EISAID found, looks up
the known ISA device table to map bytes to PCI-class equivalents:

| EISAID  | Bytes | class / subclass / prog_if | Description           |
|---------|-------|----------------------------|-----------------------|
| PNP0501 | 05/01 | 0x07/0x00/0x02             | 16550A serial         |
| PNP0303 | 03/03 | 0x09/0x00/0x00             | AT keyboard (i8042)   |
| PNP0F03 | 0F/03 | 0x09/0x02/0x00             | MS serial mouse       |
| PNP0F13 | 0F/13 | 0x09/0x02/0x00             | PS/2 mouse (i8042)    |
| PNP0700 | 07/00 | 0x01/0x02/0x00             | Floppy controller     |
| PNP0400 | 04/00 | 0x07/0x01/0x00             | Parallel port ECP     |
| PNP0401 | 04/01 | 0x07/0x01/0x01             | Parallel port EPP     |
| PNP0B00 | 0B/00 | 0x08/0x03/0x00             | CMOS RTC              |
| PNP0C04 | 0C/04 | 0x0B/0x80/0x00             | FPU/coprocessor       |
| PNP0C02 | 0C/02 | 0x08/0x80/0x00             | Motherboard resources |

Extracts I/O base from the `_CRS` resource descriptor within 512 bytes of the
EISAID. Publishes each device via `DEVMGR_PUBLISH_DEVICE` with `bus=0xFF`
marker. Sends `DEVMGR_ACPI_SCAN_DONE` after the full scan.

Physical table pages are mapped through a 16-page (64 KB) aligned WASM memory
window (`g_map_window`).

#### `fs-manager` — Virtual Filesystem Namespace Router

- **Kind:** WASM service
- **Registers:** `fs.vfs` service endpoint

Routes virtual path requests to backend endpoints. Tracks registered backends:

| Backend kind         | Value | Description             |
|----------------------|-------|-------------------------|
| `FSMGR_BACKEND_BOOT` | 1     | FAT partition (`/boot`) |
| `FSMGR_BACKEND_INIT` | 2     | Initfs image            |

Fs-manager IPC opcodes:

| Opcode                            | Value   | Meaning                       |
|-----------------------------------|---------|-------------------------------|
| `FSMGR_IPC_REGISTER_BACKEND_REQ`  | `0x420` | Register backend endpoint     |
| `FSMGR_IPC_CLONE_CWD_REQ`         | `0x421` | Clone current-directory state |
| `FSMGR_IPC_QUERY_MOUNTS_REQ`      | `0x422` | List active mount points      |
| `FSMGR_IPC_REGISTER_BACKEND_RESP` | `0x4A0` | Registration response         |
| `FSMGR_IPC_CLONE_CWD_RESP`        | `0x4A1` | Clone response                |
| `FSMGR_IPC_QUERY_MOUNTS_RESP`     | `0x4A2` | Mount list response           |

#### `font-service` — TTF Font Renderer (Native Zig)

- **Kind:** native Zig service
- **Registers:** `font` service endpoint

Loads built-in TTF fonts from `fs.vfs` and serves glyph rasterization
requests. Uses the shared `libsys` event-loop pattern with intent-based
completion for outbound FS IPC calls.

#### `gfx-compositor` — Graphics Compositor (Native Zig)

- **Kind:** native Zig service
- **Registers:** `gfx` service endpoint

Owns all display policy: window layout, z-order, surface composition, input
routing. Writes to kernel-managed shared framebuffer handles via borrow
semantics. Pixel data never flows through IPC messages. See
`docs/architecture/20-graphics-framebuffer-and-compositor.md`.

#### `vt` — Virtual Terminal Multiplexer

- **Kind:** WASM service
- **Registers:** `vt` service endpoint

Multiplexes keyboard input and text output across up to 4 TTY sessions. Supports
raw and canonical input modes.

VT IPC opcodes:

| Opcode                   | Value   | Meaning                             |
|--------------------------|---------|-------------------------------------|
| `VT_IPC_WRITE_REQ`       | `0x700` | Write text to active TTY            |
| `VT_IPC_READ_REQ`        | `0x701` | Read from active TTY                |
| `VT_IPC_SET_ATTR_REQ`    | `0x702` | Set text attributes                 |
| `VT_IPC_SWITCH_TTY`      | `0x703` | Switch active TTY                   |
| `VT_IPC_GET_ACTIVE_TTY`  | `0x704` | Query active TTY index              |
| `VT_IPC_REGISTER_WRITER` | `0x705` | Register output writer              |
| `VT_IPC_SET_MODE_REQ`    | `0x706` | Set input mode (raw/canonical/echo) |
| `VT_IPC_RESP`            | `0x780` | Response                            |
| `VT_IPC_ERROR`           | `0x7FF` | Error                               |

Input mode flags: `VT_INPUT_MODE_RAW=0`, `VT_INPUT_MODE_CANONICAL=1<<0`,
`VT_INPUT_MODE_ECHO=1<<1`.

#### `sysinit` — Late-Boot Service Launcher

- **Kind:** WASM service
- **Entry bindings:** `proc.endpoint`

Intentionally narrow: reads and executes `/boot/system/sysinit.rc`, a simple
script interpreted by the `wasmos_script` engine. Current script:

```
spawn /boot/apps/chardevc.wap
start /boot/system/services/vt.wap
if -f /boot/system/services/fontsvc.wap then
    start /boot/system/services/fontsvc.wap
endif
spawn /boot/system/services/gfxcomp.wap
start /boot/system/services/cli.wap
```

`start` spawns synchronously (waits for `PROC_IPC_NOTIFY_READY` or 5-second
timeout). `spawn` spawns asynchronously (no wait).

#### `cli` — Interactive Shell

- **Kind:** WASM service
- **Entry bindings:** `proc.endpoint`

Interactive shell over `proc` and `fs.vfs`. Commands: `help`, `ps`, `kmaps`,
`ls`, `cat`, `cd`, `mount`, `script`, `source`, `spawn`, `export`, `set`,
`echo`, `tty`, `halt`, `reboot`.

---

### Rule System

Device-manager loads udev-style rule files to drive driver selection. The rule
parser is in `src/services/device_manager/device_manager_rules.c`.

#### Rule File Locations

| Path                                      | When loaded                               | Protected                    |
|-------------------------------------------|-------------------------------------------|------------------------------|
| `/init/devmgr/rules/default.rules`        | Bootstrap; loaded from initfs             | Always; cannot be overridden |
| `/boot/system/devmgr/rules/default.rules` | After `boot_mount_ready`; loaded from FAT | Runtime override             |

Both files use the filename `DEVMGR_RULE_FILE = "default.rules"`.
Rule text buffer cap: `DEVMGR_RULE_TEXT_CAP = 1024` bytes per file.

#### Rule Families and Capacity

| Family                | Constant                    | Capacity | Triggered by         |
|-----------------------|-----------------------------|----------|----------------------|
| `boot` / always-spawn | `ALWAYS_SPAWN_RULE_CAP = 8` | 8 rules  | `SUBSYSTEM=="boot"`  |
| `block`               | `BLOCK_FS_RULE_CAP = 8`     | 8 rules  | `SUBSYSTEM=="block"` |
| PCI match             | `PCI_MATCH_RULE_CAP = 8`    | 8 rules  | `SUBSYSTEM=="pci"`   |
| ACPI match            | `ACPI_MATCH_RULE_CAP = 8`   | 8 rules  | `SUBSYSTEM=="acpi"`  |

#### Rule Syntax

One rule per line, comma-separated clauses. Comments start with `#`.

| Operator       | Meaning                |
|----------------|------------------------|
| `KEY=="value"` | Match condition        |
| `KEY="value"`  | Assign metadata output |
| `KEY+="value"` | Append action          |

Recognized keys:

| Key              | Applies to | Meaning                                |
|------------------|------------|----------------------------------------|
| `SUBSYSTEM`      | All        | `"boot"`, `"pci"`, `"block"`, `"acpi"` |
| `ATTR{class}`    | pci, acpi  | Class code (hex)                       |
| `ATTR{subclass}` | pci, acpi  | Subclass code (hex)                    |
| `ATTR{prog_if}`  | pci        | Programming interface (hex)            |
| `ATTR{vendor}`   | pci        | Vendor ID (hex)                        |
| `ATTR{device}`   | pci        | Device ID (hex)                        |
| `ATTR{bus}`      | pci        | Bus number (hex)                       |
| `ATTR{slot}`     | pci        | Slot number (hex)                      |
| `ATTR{function}` | pci        | Function number (hex)                  |
| `ATTR{unit}`     | block      | Unit number (decimal) or `"any"`       |
| `ENV{MOUNT}`     | block      | Mount alias for filesystem rule        |
| `RUN+=`          | All        | Driver/service path to spawn           |

Wildcards: `ATTR{class}` etc. that are absent default to `MATCH_ANY_U8=0xFF`
or `MATCH_ANY_U16=0xFFFF`. Unknown or malformed lines are skipped silently.

#### Shipped Rule Files

**`/init/devmgr/rules/default.rules`** (bootstrap rules):
```
SUBSYSTEM=="pci", ATTR{class}=="0x01", ATTR{subclass}=="0x01", RUN+="system/drivers/ata.wap"
SUBSYSTEM=="block", ATTR{unit}=="0", ENV{MOUNT}="/boot", RUN+="system/drivers/fs_fat.wap"
```

**`/boot/system/devmgr/rules/default.rules`** (boot-FAT override rules):
```
SUBSYSTEM=="block", ATTR{unit}=="1", ENV{MOUNT}="/user", RUN+="system/drivers/fs_fat.wap"
SUBSYSTEM=="pci", ATTR{class}=="0x03", ATTR{subclass}=="0x00", ATTR{prog_if}=="0x00", RUN+="system/drivers/fbpci.wap"
SUBSYSTEM=="acpi", ATTR{class}=="0x07", ATTR{subclass}=="0x00", RUN+="system/drivers/serial.wap"
SUBSYSTEM=="acpi", ATTR{class}=="0x09", ATTR{subclass}=="0x00", RUN+="system/drivers/keyboard.wap"
SUBSYSTEM=="acpi", ATTR{class}=="0x09", ATTR{subclass}=="0x02", RUN+="system/drivers/mouse.wap"
SUBSYSTEM=="acpi", ATTR{class}=="0x08", ATTR{subclass}=="0x03", RUN+="system/drivers/rtc.wap"
SUBSYSTEM=="pci", ATTR{class}=="0x07", ATTR{subclass}=="0x00", ATTR{prog_if}=="0x00", ATTR{vendor}=="0x1AF4", RUN+="system/drivers/virtio_serial.wap"
```

---

### Bootstrap Startup Chain

```
kernel init
  └─ spawn device-manager (initfs module index)
       ├─ spawn pci-bus   (initfs module, io.port 0xCF8–0xCFF)
       │    └─ publishes PCI device records → device-manager.inventory
       │    └─ DEVMGR_PCI_SCAN_DONE
       ├─ spawn acpi-bus  (initfs module, mmio.map)
       │    └─ publishes ISA/PNP device records → device-manager.inventory
       │    └─ DEVMGR_ACPI_SCAN_DONE
       ├─ [pci match rule] spawn ata  (PCI class 0x01/0x01)
       │    └─ registers "block" endpoint
       │    └─ DEVMGR_PUBLISH_BLOCK_DEVICE → device-manager.inventory
       ├─ [block rule, unit=0] spawn fs-fat  (mount=/boot)
       │    └─ boot_mount_ready = 1
       │    └─ queues boot-FAT "always-spawn" rules
       ├─ [boot-FAT rules loaded] spawn fs-manager
       │    └─ registers "fs.vfs" endpoint
       ├─ [boot-FAT rules] spawn framebuffer_pci, serial, keyboard, mouse, rtc, virtio-serial
       └─ PROC_IPC_NOTIFY_READY (to kernel, once boot_mount_ready + rules_boot_loaded)

kernel init (after device-manager NOTIFY_READY)
  └─ spawn sysinit  (from /boot via fs.vfs)
       └─ executes /boot/system/sysinit.rc
            ├─ spawn  chardevc.wap
            ├─ start  vt.wap
            ├─ start  fontsvc.wap  (if present)
            ├─ spawn  gfxcomp.wap
            └─ start  cli.wap
```

---

### Named Service Endpoint Registry

| Name            | Registered by    | Purpose                                    |
|-----------------|------------------|--------------------------------------------|
| `block`         | `ata`            | ATA block device                           |
| `devmgr.inv`    | `device-manager` | Device inventory endpoint for bus services |
| `devmgr.query`  | `device-manager` | Mount/block query endpoint for drivers     |
| `fs.vfs`        | `fs-manager`     | Unified virtual filesystem namespace       |
| `initfs.rules`  | `fs-init`        | Initfs listing (pre-FAT)                   |
| `virtio.serial` | `virtio-serial`  | VirtIO serial register access              |
| `font`          | `font-service`   | Font rasterization                         |
| `gfx`           | `gfx-compositor` | Graphics compositor                        |
| `vt`            | `vt`             | Virtual terminal multiplexer               |

---

### Device-Manager Design Direction

The current device-manager is a functioning bootstrap sequencer with a
rule-engine and in-memory device registry. The planned direction (from
`03-architectural-direction.md`) is a full MINIX-style device manager:

- **Bus-agnostic discovery.** PCI and ACPI already publish normalized records.
  USB and virtual providers follow the same contract.
- **Supervised driver lifecycle.** Crashed drivers should be detected, stale
  endpoints revoked, and replacements spawned. Endpoint identity preservation
  across restart is the required primitive.
- **Dynamic mount policy.** Mount assignments as rule outcomes, not
  compile-time constants. `fs-manager` remains the namespace router.
- **Unified block-device identity.** All transports normalize to
  `block:<parent-address>:<unit>` canonical IDs.

What is **not yet implemented**: driver liveness monitoring and restart,
hotplug event pipeline, endpoint identity preservation across restarts,
IRQ bind/unbind delegation to driver endpoints.

DMA-specific design details are tracked in `docs/architecture/12-dma-transfers.md`.
