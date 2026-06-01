## Device Manager and Bus Enumeration

This document describes the device manager service: its startup phase state
machine, rule engine, PCI bus scanner, ACPI bus scanner, and IPC protocol for
device inventory.

**Sources**: `src/services/device_manager/device_manager.c`,
`src/services/device_manager/device_manager_rules.c`,
`src/services/pci_bus/pci_bus.c`,
`src/services/acpi_bus/acpi_bus.c`,
`scripts/system/devmgr/rules/default.rules`

---

### Overview

The device manager is the central coordinator for hardware discovery. It:

1. Loads rule files from the filesystem during startup.
2. Spawns always-on drivers declared in the rules.
3. Receives device records from bus scanners (PCI, ACPI).
4. Matches each device against rules and spawns the appropriate driver.
5. Handles block-device registrations, routing them to filesystem mounts.

---

### Startup Phase State Machine

```c
typedef enum {
    HW_PHASE_INIT,
    HW_PHASE_SPAWN,
    HW_PHASE_WAIT,
    HW_PHASE_WAIT_INVENTORY,
    HW_PHASE_WAIT_ACPI_INVENTORY,
    HW_PHASE_IDLE,
    HW_PHASE_FAILED,
} hw_phase_t;
```

```c
typedef enum {
    HW_SPAWN_NONE,
    HW_SPAWN_RULE_PATH,
    HW_SPAWN_PCI_BUS,
    HW_SPAWN_ACPI_BUS,
    HW_SPAWN_FAT,
    HW_SPAWN_FS_INIT,
    HW_SPAWN_FS_MANAGER,
} hw_spawn_t;
```

Relevant timeout and poll constants:

```c
#define DM_SPAWN_TIMEOUT_MS    5000
#define DM_SPAWN_POLL_MAX      65536
#define DM_SPAWN_SYNC_POLL_MAX 0x7FFFFFFF
```

The manager begins in `HW_PHASE_INIT`, reads rules from the filesystem, spawns
always-on drivers in `HW_PHASE_SPAWN`, waits for their ready signals in
`HW_PHASE_WAIT`, then transitions to `HW_PHASE_WAIT_INVENTORY` (PCI scan in
progress) and `HW_PHASE_WAIT_ACPI_INVENTORY` (ACPI scan in progress) before
settling in `HW_PHASE_IDLE`.

---

### Rule Engine

**Source**: `src/services/device_manager/device_manager_rules.c`

#### Rule Types

| Kind constant                | Value | Trigger condition                     |
|------------------------------|-------|---------------------------------------|
| `RULE_SPAWN_KIND_NONE`       | 0     | Unused slot                           |
| `RULE_SPAWN_KIND_ALWAYS`     | 1     | `SUBSYSTEM=="boot"` — always spawn    |
| `RULE_SPAWN_KIND_BLOCK_FS`   | 2     | `SUBSYSTEM=="block"` + unit + mount   |
| `RULE_SPAWN_KIND_PCI_MATCH`  | 3     | `SUBSYSTEM=="pci"` + class attributes |
| `RULE_SPAWN_KIND_ACPI_MATCH` | 4     | `SUBSYSTEM=="acpi"` + class/subclass  |

Capacity per rule type:

```c
#define ALWAYS_SPAWN_RULE_CAP 8
#define BLOCK_FS_RULE_CAP     8
#define PCI_MATCH_RULE_CAP    8
#define ACPI_MATCH_RULE_CAP   8
```

Wildcard sentinel for numeric fields:

```c
#define MATCH_ANY_U8  0xFFu
#define MATCH_ANY_U16 0xFFFFu
```

#### Rule File Locations

Rules are loaded at two path roots (tried in order):

```c
#define DEVMGR_RULES_INIT_ROOT "/init/devmgr/rules"
#define DEVMGR_RULES_BOOT_ROOT "/boot/system/devmgr/rules"
#define DEVMGR_RULE_FILE       "default.rules"
```

Text buffer capacity for the combined rule file:

```c
#define DEVMGR_RULE_TEXT_CAP 1024
```

#### Rule Syntax

Each line is a comma-separated list of `key==value` tokens, optionally
quoted. `#` begins a line comment.

Example from `scripts/system/devmgr/rules/default.rules`:

```
SUBSYSTEM=="block", ATTR{unit}=="1", ENV{MOUNT}="/user", RUN+="system/drivers/fs_fat.wap"
SUBSYSTEM=="pci",   ATTR{class}=="0x03", ATTR{subclass}=="0x00", ATTR{prog_if}=="0x00", RUN+="system/drivers/fbpci.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x07", ATTR{subclass}=="0x00", RUN+="system/drivers/serial.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x09", ATTR{subclass}=="0x00", RUN+="system/drivers/keyboard.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x09", ATTR{subclass}=="0x02", RUN+="system/drivers/mouse.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x08", ATTR{subclass}=="0x03", RUN+="system/drivers/rtc.wap"
SUBSYSTEM=="pci",   ATTR{class}=="0x07", ATTR{subclass}=="0x00", ATTR{prog_if}=="0x00", ATTR{vendor}=="0x1AF4", RUN+="system/drivers/virtio_serial.wap"
```

Numeric attributes are parsed as hexadecimal (`parse_u8_hex`, `parse_u16_hex`)
or decimal (`parse_u8_dec`) depending on the field.

---

### PCI Bus Scanner

**Source**: `src/services/pci_bus/pci_bus.c`

The PCI bus service performs a full BDF (Bus:Device:Function) scan:

- 256 buses × 32 devices × 8 functions = 65 536 probes.
- Reads vendor ID, device ID, class byte, subclass byte, BAR0, and IRQ from
  PCI config space.
- Skips entries with `vendor == 0xFFFF` (no device).

Each discovered device is published to the device manager's inventory endpoint
with `DEVMGR_PUBLISH_DEVICE (0x900)`:

```
arg0 = (class << 8) | subclass
arg1 = (vendor_id << 16) | device_id
arg2 = (bus << 24) | (slot << 16) | (fn << 8) | irq_line
arg3 = bar0 (first BAR, typically I/O base or MMIO address)
```

After the full scan, sends `DEVMGR_PCI_SCAN_DONE (0x901)` to signal completion.

The scanner locates the device manager's inventory endpoint by looking up the
service `"devmgr.inv"` with 1024 retries (to handle startup ordering races).

---

### ACPI Bus Scanner

**Source**: `src/services/acpi_bus/acpi_bus.c`

#### Table Navigation

```c
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    ...
} acpi_sdt_hdr_t;

typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    ...
    uint32_t rsdt_address;
    ...
    uint64_t xsdt_address;
} acpi_rsdp_t;
```

Navigation path: `RSDP → RSDT/XSDT → FACP (FADT) → DSDT`.

Physical memory is mapped through a fixed 16-page window:

```c
#define PHYS_MAP_WINDOW_PAGES 16
#define PHYS_MAP_WINDOW_SIZE  (16 * 4096)
static uint8_t g_map_window[PHYS_MAP_WINDOW_SIZE] __attribute__((aligned(4096)));
```

#### AML Scanner and ISA PNP ID Table

The scanner walks the DSDT bytecode looking for `_HID` objects with EISAID
encoding (`DWordPrefix (0x0C)` followed by vendor bytes `0x41, 0xD0`).

Recognized PNP IDs (10 entries):

| PNP ID  | Device                    | Class | Subclass |
|---------|---------------------------|-------|----------|
| PNP0501 | Serial port (16550A)      | 0x07  | 0x00     |
| PNP0303 | AT keyboard controller    | 0x09  | 0x00     |
| PNP0F03 | PS/2 mouse (Microsoft)    | 0x09  | 0x02     |
| PNP0F13 | PS/2 mouse (Logitech)     | 0x09  | 0x02     |
| PNP0700 | Floppy controller         | 0x01  | 0x02     |
| PNP0400 | AT-compatible parallel    | 0x07  | 0x01     |
| PNP0401 | ECP parallel port         | 0x07  | 0x01     |
| PNP0B00 | CMOS RTC                  | 0x08  | 0x03     |
| PNP0C04 | Math coprocessor (FPU)    | 0x0B  | 0x80     |
| PNP0C02 | Motherboard resources     | 0x08  | 0x80     |

After the `_HID` match the scanner parses the `_CRS` (Current Resource
Settings) buffer for:
- **I/O port descriptors** (tag `0x47`): extracts `io_base`.
- **IRQ descriptors** (tag `0x22` / `0x23`): extracts `irq_line`.

#### Deduplication

A seen-array of 16 entries keyed on `(io_base << 16) | (b2 << 8) | b3`
prevents publishing the same physical device twice when both a `_HID` node
and a `_CID` alias match.

#### Publication

Each recognized ISA device is published as:

```
DEVMGR_PUBLISH_DEVICE (0x900)
  arg0 = (class << 8) | subclass
  arg1 = 0x0000FFFF   (vendor=ACPI/ISA sentinel, device=0xFFFF)
  arg2 = (0xFF << 24) | (0 << 16) | (0 << 8) | irq_line
         (bus=0xFF marks ISA bus)
  arg3 = io_base
```

Followed by `DEVMGR_ACPI_SCAN_DONE (0x905)` when the DSDT walk is complete.

---

### IPC Opcode Summary

| Opcode                        | Value | Direction            | Meaning                      |
|-------------------------------|-------|----------------------|------------------------------|
| `DEVMGR_PUBLISH_DEVICE`       | 0x900 | bus scanner → devmgr | Report one discovered device |
| `DEVMGR_PCI_SCAN_DONE`        | 0x901 | pci_bus → devmgr     | PCI scan complete            |
| `DEVMGR_PUBLISH_BLOCK_DEVICE` | 0x903 | driver → devmgr      | Register a block device      |
| `DEVMGR_ACPI_SCAN_DONE`       | 0x905 | acpi_bus → devmgr    | ACPI/AML scan complete       |

---

### Event Loop Architecture

The device manager maintains four independent event loops, each on its own
IPC endpoint:

| Loop variable          | Endpoint purpose                                         |
|------------------------|----------------------------------------------------------|
| `g_dm_ipc_loop`        | Main IPC endpoint — commands from other services         |
| `g_dm_inventory_loop`  | Inventory endpoint — device records from bus scanners    |
| `g_dm_query_loop`      | Query endpoint — device info requests                    |
| `g_dm_rules_loop`      | Rules endpoint — file reads from the filesystem manager  |

Each loop polls its own endpoint and dispatches to its own handler set. This
lets the device manager interleave rule loading with device arrival events
without blocking either path.

---

### Structural Invariants

1. **Bus scanners run after always-spawn drivers are up.** The device manager
   waits for always-spawn ready signals before transitioning to
   `HW_PHASE_WAIT_INVENTORY`, ensuring the filesystem is available for
   rule-based driver spawning.

2. **ACPI scan uses ISA bus sentinel (0xFF).** All ACPI-discovered devices
   carry `bus=0xFF` in arg2 so the device manager rule matcher can
   distinguish them from PCI devices.

3. **Rule matching uses `MATCH_ANY_U8 / MATCH_ANY_U16` wildcards.** A rule
   that omits `vendor` or `device` matches any value via the wildcard
   sentinel rather than an explicit wildcard string.

4. **Block devices trigger mount routing.** When a
   `DEVMGR_PUBLISH_BLOCK_DEVICE` arrives, the device manager checks the
   block-fs rule list and, if a matching rule exists, spawns the appropriate
   filesystem driver with the declared mount point.

5. **The inventory endpoint is separate from the command endpoint.** Bus
   scanners use `"devmgr.inv"` (not `"devmgr"`), so high-volume inventory
   floods cannot starve other service command traffic.
