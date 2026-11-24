## Device Manager and Bus Enumeration

> **Documentation status: Implemented reference with deferred lifecycle work.**

This document describes the device manager service: its startup phase state
machine, rule engine, PCI bus scanner, ACPI bus scanner, and IPC protocol for
device inventory.

**Sources**: `src/services/device_manager/device_manager.c`,
`src/services/device_manager/device_manager_rules.c`,
`src/services/pci_bus/pci_bus.c`,
`src/services/acpi_bus/acpi_bus.c`,
`scripts/system/devmgr/rules/default.<arch>.rules`

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
| `RULE_SPAWN_KIND_ACPI_MATCH` | 4     | `SUBSYSTEM=="acpi"` + class/subclass, optional `ATTR{io}` (I/O base), `ATTR{hid}` (ACPI `_HID`) and `ATTR{console}` (`1`: the firmware's console device) |

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
#define DEVMGR_RULE_TEXT_CAP 4096
```

#### Rule Syntax

Each line is a comma-separated list of `key==value` tokens, optionally
quoted. `#` begins a line comment.

Example from `scripts/system/devmgr/rules/default.<arch>.rules` (excerpt):

```
SUBSYSTEM=="block", ATTR{unit}=="1", ENV{MOUNT}="/user", RUN+="system/drivers/fs_fat.wap"
SUBSYSTEM=="pci",   ATTR{class}=="0x03", ATTR{subclass}=="0x00", ATTR{prog_if}=="0x00", RUN+="system/drivers/fbpci.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x07", ATTR{subclass}=="0x00", ATTR{io}=="0x3F8", RUN+="system/drivers/uart16550.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x09", ATTR{subclass}=="0x00", RUN+="system/drivers/keyboard.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x09", ATTR{subclass}=="0x02", RUN+="system/drivers/mouse.wap"
SUBSYSTEM=="acpi",  ATTR{class}=="0x08", ATTR{subclass}=="0x03", RUN+="system/drivers/rtc.wap"
SUBSYSTEM=="pci",   ATTR{vendor}=="0x1AF4", ATTR{device}=="0x1003", RUN+="system/drivers/virtio_serial.wap"
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

Each discovered function is published to the device manager's inventory
endpoint as a `wasmos_pci_device_desc_t` (`DEVMGR_PUBLISH_DEVICE_DESC`,
`arg0 = buffer_id, arg1 = offset, arg2 = size`), one slot per function in a
transfer buffer pci-bus owns and lends to device-manager for reading.

After the full scan, sends `DEVMGR_PCI_SCAN_DONE (0x901)` to signal completion.

The scanner locates the device manager's inventory endpoint by looking up the
service `"devmgr.inv"` with 1024 retries (to handle startup ordering races).

#### Resident Phase: Configuration-Space Owner

pci-bus does not exit after the scan. It registers the service name `"pci"`
**before** scanning — publishing devices is what makes device-manager spawn
drivers, and a driver must never find the bus service missing — and then serves
requests for the rest of the boot.

It stays resident because it is the only process that may write configuration
space (the `pci.config` capability; the kernel serialises the 0xCF8/0xCFC port
pair behind `pci_config_read` / `pci_config_write`): device-manager grants each
driver register windows covering that device's own BARs -- the `[[regions]]`
the driver declared, each `"bar"` region resolved to an I/O-port or a
memory-mapped window by the BAR's kind (`dm_resolve_declared_regions`) -- which
excludes config space. Widening that grant would give every driver read/write access to every
device on the bus.

device-manager learns what a rule's driver declares -- capability bits,
`[[regions]]`, DMA budget -- from the process manager: by boot-module index
(`PROC_IPC_MODULE_META_DESC`) for a driver in the boot image, and by path
(`PROC_IPC_MODULE_META_FILE`, read through fs-manager, answered deferred) for a
driver on a filesystem. A PCI rule is not armed until that answer is in, nor
while a freshly loaded rule set's metadata is still stale: armed earlier, its
driver would be spawned without the windows it declared. A driver that cannot
be described is spawned without declared windows, with the legacy BAR0-derived
I/O window. An MSI binding whose owner has died belongs to no one: the
kernel disabled the function's MSI and MSI-X at the owner's reap, and the next
driver to bind the function takes the binding over.

Requests are served on a **dedicated endpoint**, separate from the one used for
device-manager traffic: a reply-matching loop discards messages it did not ask
for, which is how a driver's request would otherwise vanish during a lookup.

| Opcode                | Value | Purpose |
|-----------------------|-------|---------|
| `PCI_IPC_MSI_QUERY`   | 0xd00 | `arg0 = bdf` → `arg0 = WASMOS_PCI_MSI_KIND_*`, `arg1 = vector count` |
| `PCI_IPC_MSI_BIND`    | 0xd01 | `arg0 = (bdf << 8) \| entry`, `arg1/arg2 = address`, `arg3 = data` |
| `PCI_IPC_MSI_UNBIND`  | 0xd02 | `arg0 = (bdf << 8) \| entry` |
| `PCI_IPC_BUS_MASTER_REQ` | 0xd03 | `arg0 = bdf` → bus mastering on, for a driver that takes no MSI |
| `PCI_IPC_INTX_ENABLE_REQ` | 0xd04 | `arg0 = bdf` → bus mastering on and `INTX_DISABLE` cleared, for a driver on its INTx fallback |

`MSI_BIND` walks the capability list (`0x11` MSI-X, preferred; `0x05` MSI),
writes the address/data pair the caller obtained from `wasmos_msi_alloc` into the
device, then sets bus-mastering (a message write is DMA) and `INTX_DISABLE` (so
the device stops driving its shared wire). A function is claimed by the first
endpoint that binds it and refuses a different one afterwards, so a driver cannot
redirect another device's interrupts to itself. See
`docs/architecture/05-x86-cpu-architecture.md` §Message-Signalled Interrupts for
the full ownership split.

MSI-X is reported in preference to MSI because it addresses each vector
independently; plain MSI needs one naturally-aligned block of consecutive
vectors, which the kernel allocator does not hand out, so it is reported as
exactly one vector.

Staying resident is also the prerequisite for hot-plug: a rescan opcode over the
scan function is a small addition, but knowing *when* to rescan needs an ACPI
GPE/SCI path that does not exist yet.

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

Each table is mapped with `wasmos_phys_map(phys_lo, phys_hi, size)`, which
maps the firmware ACPI memory holding it into acpi-bus's linear memory and
returns a pointer to the table: first its header, then its declared length,
which resolves to the same window when the header's page already covers it.
The windows stay mapped for the life of the service, one or two per table.

Navigation also notes the SPCR (Serial Port Console Redirection) table when the
firmware provides one: its Generic Address Structure names the console UART's
address space and base, which the scanner compares against each device's
register windows.

#### AML Scanner and Device Table

The scanner walks the DSDT bytecode for `Name (_HID, ...)` objects in either
encoding: an EISAID (`DWordPrefix`), converted to its string form ("PNP0501"),
or a string (`StringPrefix`, "ARMH0011"). A recognised `_HID` maps onto the PCI
class a rule matches:

| `_HID`   | Device                    | Class | Subclass | prog_if |
|----------|---------------------------|-------|----------|---------|
| PNP0501  | 16550A serial             | 0x07  | 0x00     | 0x02    |
| ARMH0011 | Arm PL011 UART            | 0x07  | 0x00     | 0x80    |
| PNP0303  | AT keyboard (i8042)       | 0x09  | 0x00     | 0x00    |
| PNP0F03  | MS serial mouse           | 0x09  | 0x02     | 0x00    |
| PNP0F13  | PS/2 mouse (i8042)        | 0x09  | 0x02     | 0x00    |
| PNP0700  | Floppy controller         | 0x01  | 0x02     | 0x00    |
| PNP0400  | Parallel port ECP         | 0x07  | 0x01     | 0x00    |
| PNP0401  | Parallel port EPP         | 0x07  | 0x01     | 0x01    |
| PNP0B00  | CMOS RTC                  | 0x08  | 0x03     | 0x00    |
| PNP0C04  | FPU/coprocessor           | 0x0B  | 0x80     | 0x00    |
| PNP0C02  | Motherboard resources     | 0x08  | 0x80     | 0x00    |

The `_CRS` resource template after the `_HID` is walked by each descriptor's
encoded length (ACPI 6.5 §6.4): every I/O Port and Fixed I/O Port descriptor
and every 32-bit Fixed Memory Range becomes a register window, in template
order, and the first IRQ or Extended Interrupt descriptor is the device's
interrupt (an ISA line, or a GIC GSIV on aarch64). A device that lists no
window of its own (the PS/2 mouse behind the i8042) takes the table's default
I/O window.

#### Publication

Each recognised device is published as a `wasmos_acpi_device_desc_t`
(`DEVMGR_PUBLISH_ACPI_DEVICE_DESC`, `arg0 = buffer_id, arg1 = offset, arg2 =
size`): the `_HID`, the class triple, up to four register windows as region
grants, the interrupt identifier, and `WASMOS_ACPI_DEVICE_CONSOLE` when one of
its windows is the SPCR console's. acpi-bus owns the descriptor buffer and lends
it to device-manager for reading, one slot per device; it therefore stays
resident after `DEVMGR_ACPI_SCAN_DONE (0x905)`, because its exit would destroy
the buffer while device-manager may still be reading descriptors from it.

Device-manager records an ACPI device with `bus = 0xFF`, its windows in
`bars[]` and its first I/O window's base in `io_port_base` (what `ATTR{io}`
matches). An ACPI-matched driver is granted those windows as region grants in
the same order (an MMIO window rounded out to whole pages) and the interrupt,
and is spawned with `hid=`, `io=` or `mmio=` and `irq=`. The firmware describes
the windows; the driver's own module descriptor contributes only DMA: a driver
whose manifest declares `dma.buffer` with a page budget is also granted
`DEVMGR_CAP_DMA` and that budget (`dm_acpi_caps`). An ACPI rule's descriptor is
always fetched with the deferred `PROC_IPC_MODULE_META_FILE` request, and the
rule waits for the answer before it is armed, as a PCI rule does. When the firmware
names a console (an SPCR exists, `WASMOS_ACPI_DEVICE_CONSOLE_KNOWN`), every
ACPI device is also told whether it is that console (`console=1` or
`console=0`); without an SPCR no `console=` is passed and a driver keeps its
own default (COM1 for the 16550 backend). A rule binds the firmware's choice
with `ATTR{console}=="1"`, and a device that several ACPI rules select gets one
driver (`dm_acpi_device_driven`).

### IPC Opcode Summary

| Opcode                        | Value | Direction            | Meaning                      |
|-------------------------------|-------|----------------------|------------------------------|
| `DEVMGR_PCI_SCAN_DONE`        | 0x901 | pci_bus → devmgr     | PCI scan complete            |
| `DEVMGR_PUBLISH_BLOCK_DEVICE` | 0x903 | driver → devmgr      | Register a block device      |
| `DEVMGR_ACPI_SCAN_DONE`       | 0x905 | acpi_bus → devmgr    | ACPI/AML scan complete       |
| `DEVMGR_PUBLISH_DEVICE_DESC`  | 0x906 | pci_bus → devmgr     | One PCI function (descriptor) |
| `DEVMGR_PUBLISH_ACPI_DEVICE_DESC` | 0x90A | acpi_bus → devmgr | One ACPI device (descriptor) |

`0x900` (`DEVMGR_PUBLISH_DEVICE`, the packed four-word form) is retired.

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

6. **Rule tables are bounded and say so.** The PCI and ACPI rule tables hold
   32 rules each (always-spawn and block rules, 8). A rule set larger than its
   table loads up to the table, and the device manager logs how many rules were
   left out ("<n> pci rule(s) did not fit the rule table") rather than dropping
   them unseen. Several rules may match one device; each spawns its driver.

7. **A spawned driver's first outcome decides the rule for that device.**
   READY retires the rule for the device. An exit with a failure status before
   READY is a failed spawn and is retried, up to a limit, after which the device
   is given up on ("gave up on <path>"). An exit with status 0 before READY
   means the driver looked and found nothing to drive -- the ramfb driver on a
   machine with fw_cfg but no ramfb -- so the device is retired at once
   ("<path> declined the device") instead of retried. A driver that ends after
   READY is not the device manager's to restart; a supervised unit
   (`ENV{RESTART}`) is.
