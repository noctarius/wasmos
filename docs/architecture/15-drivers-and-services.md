## Drivers and Services

> **Documentation status: Mixed reference and proposal.** Existing startup,
> discovery, and driver contracts are reference material; supervision and
> lifecycle expansion remain future work.

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
| `0xC00–0xCFF` | hrng (hardware entropy)                            |

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

**Transfers are PIO but completion is interrupt-driven.** IDENTIFY runs polled
during probe; the driver then routes IRQ 14 (the primary channel's fixed ISA
line — the PIIX IDE function exposes no PCI interrupt pin, so it is not
discoverable from the device record) and clears `nIEN` in Device Control
(`0x3F6`) so the drive may assert INTRQ at all. Sector waits then block on a
dedicated interrupt endpoint instead of spinning on the status register.

Two properties keep this from becoming a boot-path dependency:

- Every wait reads status **before** sleeping, so a condition that is already
  true (a pre-command idle check, which has no interrupt coming) never waits.
- If the line is routed but no interrupt ever arrives, the driver abandons the
  interrupt after a bounded number of empty sleeps, quiets the drive, drops the
  route, and reverts to polling for the rest of the boot. Failure to route or to
  clear `nIEN` is likewise non-fatal — the driver behaves exactly as it did
  before.

Servicing an interrupt owes two things and both matter: reading the status
register de-asserts INTRQ at the drive, and `irq_ack` reopens the line the
kernel masked on dispatch. An event that arrives and is never acked leaves the
line masked permanently.

Real DMA is **not** implemented: there is no bus-master IDE (BMIDE/PRD)
programming, and the sector loop is PIO on every path. The `dma_map_borrow`
zero-copy borrow fast path is separately disabled pending a block protocol that
carries the buffer grant (`TODO(xfer-buffer owner-push)` in `ata.c`); the driver
reports its transfer mode once at startup rather than logging a per-request
"fallback", which used to read like an intermittent failure.

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

#### `virtio-rng` — VirtIO Hardware Entropy Driver

- **Kind:** WASM driver
- **PCI match:** vendor `0x1AF4`, device `0x1005` (transitional) or `0x1044`
  (modern)
- **Capabilities:** `mmio.map`, `irq.route`, `dma.buffer`, `svc.class`
- **Regions:** region 0 = BAR 4 (`kind = "bar"`), a memory-mapped window
- **Registers:** concrete service `virtio-rng`, class `hrng`, instance `0`
- **Entry bindings:** `proc.endpoint`

The driver speaks the virtio 1.x PCI transport (`wasmos/virtio_modern.h`): it
finds its common-config, notify and ISR structures in its own function's
capability list (`virtio_pci_caps.h`, read through `pci_config_read`, which a
driver may use for the function its spawn profile names) and reaches them
through its BAR 4 window with the `io_region_*` host calls. A device that
places them in another BAR is refused. It touches no I/O port.

The driver owns one request virtqueue and a 4096-byte DMA pool. A client grants
the driver write access to a transfer buffer and sends `HRNG_IPC_GET_BYTES_REQ`.
The driver completes the virtqueue request, copies produced entropy into that
buffer, and replies asynchronously.

| Opcode                   | Value   | Meaning                                      |
|--------------------------|---------|----------------------------------------------|
| `HRNG_IPC_GET_BYTES_REQ` | `0xC00` | `arg0=buffer_id`, `arg1=requested byte count` |
| `HRNG_IPC_RESP`          | `0xC80` | `arg0=bytes written`                         |
| `HRNG_IPC_ERROR`         | `0xCFF` | `arg0=HRNG_STATUS_*`                         |

`HRNG_MAX_BYTES_PER_REQ` is 4096. `libsys` splits larger byte-array requests
through its event-loop intent mechanism.

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

Coordinates early hardware startup in user space. A critical unit of the
supervisor's boot chain, spawned from its boot module after fs-manager,
fs-tmpfs and fs-init. Manages four event loops:

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
- **Capabilities:** `pci.config`, `irq.route`, `mmio.map`
- **Entry bindings:** (receives `proc_endpoint` as first arg)

Performs a full PCI configuration space scan through the kernel's
`pci_config_read` / `pci_config_write` host calls, which hold a lock across the
mechanism-1 address/data port pair (0x0CF8/0x0CFC) so no access interleaves
with another, nor with the kernel's quiesce of a dead driver's function.

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

| Backend kind          | Value | Description                            |
|-----------------------|-------|----------------------------------------|
| `FSMGR_BACKEND_BLOCK`  | 1     | Served from a block device (any volume) |
| `FSMGR_BACKEND_PSEUDO` | 2     | Served from anything else (initfs today) |

A kind says what a backend sits on, not which filesystem it serves and not which
volume it is: one `BLOCK` backend registers per mounted volume, and the
filesystem is the backend's `FS_TYPE_*` reported separately.

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

#### `supervisor` — Service Supervisor

- **Kind:** WASM service on the libsys event loop (`src/services/supervisor`)
- **Registers:** `supervisor`
- **Spawned by:** kernel init, from its boot module, right after the process manager
- **Critical:** its own end is a kernel panic (see below)

The supervisor owns one fact for every unit handed to it: *it should be
running*. It spawns the unit, watches it, and restarts it or gives up on it by
the unit's policy. Nobody else restarts anything: device-manager keeps matching
devices to drivers and sysinit keeps reading its script, and both hand the
result to the supervisor instead of growing restart logic of their own. Policy
lives here, in user space; the kernel and the process manager supply only the
mechanism -- watched spawns, exit reasons, kill (see
[Process Model and IPC](09-process-and-ipc.md), "Watched Spawn").

**The boot chain is its own.** fs-manager, fs-tmpfs, fs-init and
device-manager are units the supervisor records itself, as `critical`, before
it takes any submission, and spawns in that order from their boot modules: the
unit path `module:<app name>` is a watched spawn with
`SPAWN_WATCH_FLAG_MODULE`, built from the in-memory image, since no filesystem
exists to read one from yet. fs-tmpfs precedes fs-init and device-manager
because mount points are its directories; nothing else depends on the order.
Their death is named like any critical unit's and ends the system; nothing
stands in for the filesystem or for device-manager.

**It is a separate service, not sysinit**, for three reasons. It must exist
before the first thing it supervises, and sysinit is read from `/boot` long
after device-manager has started drivers. It must never block, and sysinit is
a script runner whose commands wait. And sysinit is not a second bootstrap
coordinator (doc 03, invariant 1): it stays a client that turns `sysinit.rc`
into submissions.

**Units.** A unit is submitted with `SUPERVISOR_IPC_SUBMIT_REQ` as one spec
line, in a descriptor the submitter owns and lends READ:

```
<unit> <path> [restart=never|on-failure|always] [class=optional|required|critical]
              [tty=<slot>] [probe=<seconds>] [-- <args>]
```

`<path>` is absolute, or `module:<app name>` for a boot module, which takes no
args and no tty pin.

A unit name is `template` or `template@instance`. The instance key names what
the unit serves -- a tty (`cli@tty1`), a device (`rtc@acpi:0070`) -- and is
derived from it, never allocated, so a restarted instance keeps its name. The
descriptor may carry a capability descriptor (I/O windows, IRQ lines, a DMA
window), which every spawn of the unit carries. A unit that asks for DMA must
name its PCI function (`DEVMGR_CAP_PCI_FUNCTION`) or it is refused
(`WASMOS_ERR_SUPERVISOR_DMA_CAPS`): a crashed driver may leave its device
bus-mastering, and only a named function is quiesced by the kernel at the
driver's end -- before its frames are recycled and before the supervisor hears
of the exit ([Capability and Policy](10-capability-and-policy.md), "Device
Quiesce"). The table holds `SUPERVISOR_UNITS_MAX` units.

**Lifecycle.** Every spawn is a `PROC_IPC_SPAWN_WATCHED` whose notify
endpoint is the supervisor's own, one in flight at a time, so the spawn reply
precedes every event of the child. A unit moves through

```
spawn-due -> spawning -> starting --READY--> running --stable--> stable
                            |                    |                  |
                            +------ end ---------+------------------+--> by policy:
                                                                        spawn-due (restart)
                                                                        stopped | failed
```

and `stopping` once the supervisor has killed it. The policy
(`supervisor_unit.c`, host-tested) is plain data and decides every transition:

- **Failure** is any end other than a clean exit with status 0, judged by the
  exit reason (`PROC_EXIT_REASON_*`), and any end of an instance the
  supervisor killed. `on-failure` (the default) restarts failures only,
  `always` restarts every end, `never` none.
- **READY** means the unit's endpoint is live, nothing more. A unit with no
  READY within `ready_timeout_ms` (10 s) is killed, as a failure.
- **Stable** is READY plus `stable_ms` (10 s) survived, plus one liveness probe
  answered when the unit is probed. Only a stable run resets the backoff.
- **Backoff** before a restart starts at 250 ms, doubles, and is capped at
  30 s.
- **Restart intensity** is a sliding window: the fifth counted end within
  60 s gives up and the unit is `failed`, with one log line naming it and its
  last end. A stable run does not wipe the window; old ends age out of it.
- Every number is multiplied by kenv `supervisor.timescale` (1-100), which
  slow configurations raise instead of tuning each timeout.

**Liveness.** A unit opts in by naming its event-loop endpoint in its READY
(`PROC_READY_FLAG_LIVENESS`). The supervisor sends `WASMOS_IPC_HEALTH_REQ` to it
every probe period (`probe=<s>`, default 10 s), one at a time, and the event loop
itself answers ([libsys](14-libsys-and-service-runtime.md)), so a loop parked in
a nested receive or stuck in a handler does not. A probe still unanswered when
the next one is due is a miss; three in a row and the unit is killed, a failure
like any other. Liveness is only "the loop turns": a unit waiting for its
dependencies is healthy, and is never killed for it.

**Classes.** `optional` (the default) and `required` units are restarted by
their policy; a required one also holds the boot milestone. A `critical` unit
is never restarted: its first end logs `[supervisor] critical unit <unit> died
(...)`, dumps the unit table to the kernel log, and runs the orderly shutdown,
then halts -- or reboots, with kenv `supervisor.on_critical=reboot`. A halt is
the default because it keeps the evidence. The supervisor itself is a critical
process in the kernel's sense (`process_set_critical`, set by kernel init): its
end is a kernel panic naming it, since nobody is left to run a shutdown.

**Boot milestone.** sysinit sends `SUPERVISOR_IPC_BOOT_SUBMITTED_REQ` when its
script has run; the units held then are the boot set. When every required or
critical one is *up* -- READY, and a probe answered when probed; not stable,
which would add 10 s to every boot -- the supervisor logs `[supervisor] boot
complete at <ms> ms: <n> required units up` once, or `[supervisor] boot
failed: required unit <unit> ...` when one fails first. This line, not "the
shell printed a prompt", is what the test harness waits for.

**Operator view.** `SUPERVISOR_IPC_LIST_REQ` writes one
`wasmos_supervisor_unit_record_t` per unit into a buffer the caller owns.
`SUPERVISOR_IPC_KILL_REQ` kills a unit's running instance by name; the end is
a failure and the policy applies. The `svc` utility wraps them: `svc submit
<spec>`, `svc list`, `svc status <unit>`, `svc kill <unit>`.

**Access.** Submitting, killing and declaring the boot set require the
sender to hold `svc.control` (sysinit, device-manager, `svc`); listing is open.
A submission that carries a capability descriptor also requires `spawn.grant`,
because the grants reach the process manager as the supervisor's own watched
spawn: the supervisor relays them only from a submitter the process manager
would accept them from directly (device-manager). The supervisor learns what
the sender holds from the kernel (`ipc_endpoint_has_cap` on the request's
source endpoint); see [Capability and Policy](10-capability-and-policy.md),
"Requester Checks".

**Clients rebind; endpoints are not preserved.** A restarted unit is a new
process with a new endpoint, registered under the same name. Its clients see
`WASMOS_IPC_HANGUP` from the old one and rebind through a name watch (the vt
for `kbd`, the compositor for `mouse`), or look the name up per request (`date`
for the clock). A client whose session cannot be rebuilt in place ends with
its provider instead: a shell whose vt hangs up exits with `WASMOS_PEER_GONE`,
and the supervisor (`cli@tty1`, `restart=always`) or the new vt starts a fresh
one. Endpoint identity across a restart would preserve nothing a
client could use: open files, borrows and sessions are gone with the old
process either way.

A synchronous call whose server ends before answering returns
`WASMOS_PEER_GONE` (`wasmos_ipc_call`, `wasmos_sys_ipc_call_native`, the
AssemblyScript `ipc.call`): it registers an await on the server first
(`ipc_peer_await`, [Process Model and IPC](09-process-and-ipc.md), "Peer
Hangup"), so the server's end is reported on the reply endpoint even if it
never answered, and the call stops on that hangup instead of discarding it
with the other stray messages. A client of a restarted provider cannot hang on
its last request.

What each client of a supervised provider does when it restarts:

| Provider | Client | On the provider's restart |
|---|---|---|
| `vt` | `cli@tty1` | exits `WASMOS_PEER_GONE`; restarted (`restart=always`) |
| `keyboard`, `mouse` | vt, gfxcomp | rebind through name watches on `kbd` / `mouse` |
| `fontsvc` | gfxcomp | rebinds through a name watch on `font` |
| `fontsvc` | libui apps | the next text call fails `WASMOS_PEER_GONE`; the window closes (`ui_context_t.peer_gone`) and the app exits |
| `gfxcomp` | libui apps | the hangup on the event endpoint closes the window; the app exits `WASMOS_PEER_GONE` |
| `gfxcomp`, `fontsvc` | `menu_bar` | exits as above; a unit with `restart=always`, so it comes back |
| `rtc` | `date` | looks it up per run |
| `rtc` | `menu_bar` | follows `rtc` by name watch; a failed read drops the endpoint until the next binding |
| `virtio_rng@pci:*` | entropy users | look up `virtio-rng` / `hrng` per request |
| `logger` | (none) | a restart continues `/var/log/messages` from the position kept in kenv `logger.seq` |
| boot chain | everyone | `critical`: never restarted, its end halts |

calculator and gfx_smoke are demo and test apps started once with `spawn`; they
exit with the compositor and are not started again.

| Opcode | Value | Meaning |
|---|---|---|
| `SUPERVISOR_IPC_SUBMIT_REQ` | `0x1000` | Hand a unit over; answered once recorded, before it is spawned |
| `SUPERVISOR_IPC_LIST_REQ` | `0x1001` | Write the unit table into the caller's buffer |
| `SUPERVISOR_IPC_BOOT_SUBMITTED_REQ` | `0x1002` | The boot set is complete |
| `SUPERVISOR_IPC_KILL_REQ` | `0x1003` | Kill a unit's instance; its end is a failure |

Not built: operator stop/start/restart that does not count as a failure;
retraction of a submitter's units when it dies; restart cascade along `[deps]`
`needs`; liveness for the boot services; supervising the DMA drivers whose
clients hold state a restart loses (virtio-blk, ata, virtio-net); a guard
against reboot loops under `on_critical=reboot`. They are tracked in `docs/TASKS.md`.

#### `sysinit` — Late-Boot Service Launcher

- **Kind:** WASM service on the libsys event loop
- **Entry bindings:** `proc.endpoint`

Intentionally narrow: reads `/boot/system/sysinit.rc` and runs it with the
`wasmos_script` engine, resumably, from its event loop. The script hands the
long-running services to the supervisor:

```
service <unit> <path> [restart=...] [class=...] [tty=<slot>] [-- args]
```

finds the supervisor by name watch, and continues once the unit is accepted --
not once it is up -- so no line waits for a service. `spawn` starts a process
without supervising it (gfx_smoke, calculator, chardevc), `exec` runs one to its end
(`mkdir -p /var/log`), and `wait-svc <name>` waits, bounded, for a name to be
registered. At the end of the script sysinit sends
`SUPERVISOR_IPC_BOOT_SUBMITTED_REQ` and stays parked on its event loop.

Every service the script starts follows its peers by name, so the order of the
`service` lines is not a correctness property. Kernel init spawns sysinit once
fs-manager reports `/boot` mounted (`FSMGR_IPC_MOUNT_WAIT_REQ`), the one real
precondition: its image lives there.

#### `logger` — Kernel Log to `/var/log/messages`

- **Kind:** WASM async service (`async_initialize`, `src/services/logger`)
- **Entry bindings:** `proc.endpoint`

Appends the kernel log to `/var/log/messages` on the tmpfs root. It pages the
kernel's kmsg ring with `kmsg_read` from the oldest retained byte, so the file
starts with the boot, and subscribes its reply endpoint with `kmsg_subscribe`,
so the kernel wakes it with `WASMOS_IPC_KMSG_NOTIFY` once per batch of appends
([Diagnostics](25-diagnostics-status.md)). Between batches it is parked on its
event loop. Opens and writes go to fs-manager as futures
(`wasmos_sys_wasm_fs_*_async`), and the ready notification the process manager
waits for is a future too: the service makes no blocking call.

Kernel log the ring overwrote before it was read is replaced by a
`[logger] N bytes ... lost` line. The file is capped at 1 MiB written per
logger instance and then truncated, with a line saying so. A write that fails
ends the service with one kernel-log line rather than a retry, because its own
message would wake it into the same failure.

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
Rule text buffer cap: `DEVMGR_RULE_TEXT_CAP = 4096` bytes per file.

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
| `ENV{RESTART}`   | acpi       | `never`, `on-failure` or `always`: hand the driver to the supervisor |
| `RUN+=`          | All        | Driver/service path to spawn           |

Wildcards: `ATTR{class}` etc. that are absent default to `MATCH_ANY_U8=0xFF`
or `MATCH_ANY_U16=0xFFFF`. Unknown or malformed lines are skipped silently.

An ACPI or PCI rule with `ENV{RESTART}` is not spawned by device-manager: it
is submitted to the supervisor as the unit `<template>@acpi:<io base>`
(`rtc@acpi:0070`) or `<template>@pci:<bus>:<device>.<function>`
(`virtio_rng@pci:00:04.0`), with the rule's arguments and the grants a direct
spawn would carry -- for a PCI driver including its function, which the kernel
quiesces whenever the driver ends -- and the supervisor's reply retires the
rule. Any other value rejects the line rather than running the driver
unsupervised. The shipped rules set `on-failure` for rtc, keyboard, mouse and
virtio-rng, whose clients hold no session a restart loses. A rule without the
key spawns directly, as does one whose submission cannot be sent because no
supervisor is bound yet; a PCI driver spawned directly by path is not quiesced
at its end, because the packed path form cannot name its function.

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
SUBSYSTEM=="pci", ATTR{vendor}=="0x1AF4", ATTR{device}=="0x1005", RUN+="system/drivers/virtio_rng.wap"
SUBSYSTEM=="pci", ATTR{vendor}=="0x1AF4", ATTR{device}=="0x1044", RUN+="system/drivers/virtio_rng.wap"
```

---

### Bootstrap Startup Chain

```
kernel init
  ├─ process-manager            (kernel-hosted)
  └─ supervisor                 (boot module; marked critical)
       ├─ fs-manager            critical unit, from its boot module; "fs.vfs"
       ├─ fs-tmpfs              critical; the tmpfs root, mount points are its directories
       ├─ fs-init               critical; the initfs listing
       └─ device-manager        critical
            ├─ pci-bus, acpi-bus     publish device records -> devmgr.inv
            ├─ initfs rules          ata, fs-fat mount=/boot, net-stack, ...
            └─ /boot rules           drivers spawned directly, or (ENV{RESTART})
                                     submitted to the supervisor as units

kernel init (once fs-manager answers FSMGR_IPC_MOUNT_WAIT_REQ for /boot)
  └─ sysinit                    from /boot
       └─ /boot/system/sysinit.rc
            ├─ service vt, logger, fontsvc, gfxcomp, menu_bar, cli@tty1  -> supervisor
            ├─ spawn chardevc, gfx_smoke, calculator; exec mkdir -p /var/log
            └─ SUPERVISOR_IPC_BOOT_SUBMITTED_REQ  -> "[supervisor] boot complete"
```

Only the filesystem itself -- fs-manager, fs-tmpfs and fs-init -- has to be
started from boot modules (`SPAWN_WATCH_FLAG_MODULE`): nothing can be read by
path before it exists. device-manager may be started either way. Every driver a
rule names is spawned by PATH through fs-manager, from `/init` before `/boot`
is mounted and from `/boot` afterwards, so a driver the boot depends on must be
in the initfs, image and rule both. A PCI or ACPI rule driver is spawned with
`PROC_IPC_SPAWN_WATCHED` (path, startup args, and a capability descriptor with
every declared window and the PCI function); device-manager does not wait on
it, and retires the rule when the child's `PROC_IPC_CHILD_READY` arrives,
retries it when the child exits first, and gives the device up when neither
happens within 5 s -- a driver still running is never spawned a second time.
pci-bus and acpi-bus are still started by boot-module index.

Kernel init waits for exactly one thing: `/boot` mounted, because sysinit's
image is on it. It subscribes to `fs.vfs`, sends the mount wait to fs-manager,
and panics with `init_no_boot_mount` if no answer comes within 60 s of the
supervisor's spawn. device-manager's READY says its endpoints are live, like
every service's; it is not a milestone anyone waits on.

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
| `virtio-rng`    | `virtio-rng`     | VirtIO hardware entropy provider           |
| `font`          | `font-service`   | Font rasterization                         |
| `gfx`           | `gfx-compositor` | Graphics compositor                        |
| `vt`            | `vt`             | Virtual terminal multiplexer               |
| `supervisor`    | `supervisor`     | Unit submission, list and kill             |

---

### Device-Manager Design Direction

The current device-manager is a functioning bootstrap sequencer with a
rule-engine and in-memory device registry. The planned direction (from
`03-architectural-direction.md`) is a full MINIX-style device manager:

- **Bus-agnostic discovery.** PCI and ACPI already publish normalized records.
  USB and virtual providers follow the same contract.
- **Supervised driver lifecycle.** device-manager matches; the supervisor
  restarts. A rule names a restart policy and device-manager submits the
  driver as a unit (see "Rule Syntax", `ENV{RESTART}`). Clients rebind by name
  on hangup; endpoint identity is not preserved across a restart (see the
  supervisor). A DMA-capable PCI driver is supervised with its function in
  its grants, which the kernel quiesces at the driver's end; virtio-rng is.
  virtio-blk, ata and virtio-net stay device-manager-spawned until their
  clients' rebind is decided (mounts, open files, sockets).
- **Dynamic mount policy.** Mount assignments as rule outcomes, not
  compile-time constants. `fs-manager` remains the namespace router.
- **Unified block-device identity.** All transports normalize to
  `block:<parent-address>:<unit>` canonical IDs.

What is **not yet implemented**: supervision of the storage and network DMA
drivers, liveness probes for drivers, retraction of a unit on device
removal (hotplug), IRQ bind/unbind delegation to driver endpoints.

DMA-specific design details are tracked in `docs/architecture/12-dma-transfers.md`.

---

### Orderly Shutdown

Shutdown is the counterpart of the readiness handshake, not a new mechanism.
Startup is `spawn` -> `PROC_IPC_NOTIFY_READY` -> the spawner unblocks; shutdown is
a notification -> the participant quiesces -> it answers. The process manager owns
both, because it already owns process lifecycle, the service registry, and the
spawn order.

```
halt/reboot  ->  kernel_system_shutdown  ->  PM steps the sequence  ->  poweroff
                  |
                  +-- WASMOS_IPC_SHUTDOWN_REQ (arg0 = halt | reboot)  -> participant
                  +-- WASMOS_IPC_SHUTDOWN_DONE                        <- participant
```

**The opcodes sit above every subsystem range** (`abi/opcodes.yaml`, subsystem
`system`, 0xff00-0xffff), because the request arrives on the PARTICIPANT's own
endpoint. A subsystem-scoped value would collide there -- `gfx` and
`proc_manager` both own 0x200 -- which is the same reason the kernel's IRQ
(0xff00) and MSI (0xff01) events already live in that band.

**Ordering is the substance of the design.** A participant may need its
dependencies alive while it quiesces: `fs-wfs` writes its superblock through the
block driver, which needs the ATA driver. Shutdown therefore runs in reverse
dependency order.

- **Declared dependencies** decide it. Each participant comes before every
  participant it needs, directly or through processes that are not
  participants themselves. "Needs" are the `[deps]` `needs` records in each
  `.wap` ([Runtime and Packaging](13-runtime-and-packaging.md)), resolved to the
  processes holding the named service or class (`shutdown_order.c`,
  host-tested). The PM logs the result, `[pm] shutdown: order N: <name> (pid)`.
- **Reverse spawn order** breaks ties: among participants free to go next the
  highest pid goes first, and a cycle of needs is broken the same way. `pid` is
  monotonic and never reused within a boot, so descending pid is reverse spawn
  order. Start order is not a dependency order once units start concurrently,
  which is why it is only the tie-break.

**Participation is opt-in**, declared as `WASMOS_SVC_FLAG_WANTS_SHUTDOWN` in the
`flags` field of `svc_register_desc_t`. The service registry supplies both the
filter and the endpoint to send to; an app registers no service name, so registry
membership already excludes the kinds that hold nothing.

Opt-in follows from the ordering. Notifying participants one at a time means each
costs its deadline, so a participant with nothing to persist would spend one
saying so. Of the registered services exactly one (`virtio_blk`) runs on the
shared async runner that could answer for free, so notifying all of them would
add roughly a minute to every halt. A service declares the flag when something it
holds must reach a device before power is cut. That is `fs-wfs` today.

**The sequence is bounded and best-effort.** Each participant has a deadline
(`WASMOS_PM_SHUTDOWN_DEADLINE_MS`); one that does not answer is passed over and
the machine still halts. This is safe because of the flag it exists to set: an
unflushed WFS volume mounts read-only on the next boot rather than serving
inconsistent metadata, so a missed shutdown costs writability, never integrity.
Best-effort shutdown is possible *because* crash safety does not depend on it.

**The halting process does not drive the sequence.** The replies arrive on the
PM's own endpoint and only its dispatch loop drains them, so the sequence is a
state machine stepped once per dispatch (`process_manager_shutdown.c`); blocking
inside a step would deadlock against the reply it blocks for. The host call arms
the request and yields until the PM powers the machine off, giving up and
powering it off itself if the sequence stops making progress -- halt always
halts.

**The console is written out before the power goes.** Once a vt owns the
console, kernel output reaches COM1 only when the vt drains its console ring,
and the sequence's last lines (a participant's result, `[pm] shutdown: complete`)
are written just before the power-off. After the last participant the PM keeps
stepping until `console_ring_drained()` reports the ring empty, bounded by
`WASMOS_PM_SHUTDOWN_DRAIN_MS`, so a vt that has stopped delays the halt but never
prevents it. The halting process's own fallback power-off does not wait: it runs
only when the sequence has already stalled.

Per-participant obligations:

| participant | on `SHUTDOWN_REQ` |
|---|---|
| `fs-wfs` | write the superblock with `state = WFS_STATE_CLEAN` and the free counters reconciled against the bitmaps |
| `fs-fat` | flush any dirty sector and the FSInfo free count -- not declared yet, tracked in `docs/TASKS.md` |
| everything else | does not declare the flag, and is not notified |

The block drivers beneath `fs-wfs` need no obligation: the filesystem goes
down first, and its last write has completed before it answers, so
there is nothing in flight by the time the drivers would be asked.

The WFS obligation subsumes what was a separate gap: the superblock's
`free_blocks` trailed the bitmaps because no path wrote it back. `wfs_sync_task`
is that path, and the clean unmount is its caller.
