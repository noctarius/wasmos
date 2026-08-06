/* pci_bus.c - WASM service: owns PCI configuration space.
 *
 * Two jobs. It enumerates config space and publishes every function to
 * device-manager (DEVMGR_PUBLISH_DEVICE), and it then STAYS RESIDENT serving
 * PCI_IPC_MSI_* requests, because programming a device's MSI/MSI-X capability is
 * a config-space write and no driver can make one: device-manager grants a
 * driver an I/O-port window covering its own BAR only, which excludes the
 * 0xCF8/0xCFC config ports. Widening that grant would give every driver
 * read/write access to every device on the bus, so the bus driver does it
 * instead.
 *
 * The split with the kernel: the kernel owns the interrupt vector namespace and
 * hands a driver an address/data pair (wasmos_msi_alloc); this service writes
 * that pair into the device. Neither side can do the other's half, which is what
 * keeps the vector binding tied to an endpoint the requesting driver owns.
 *
 * The request loop blocks in wasmos_ipc_select_one and never spins. That is
 * sufficient while every request is answered from config space without calling
 * anyone else.
 * TODO: move this loop onto the coroutine/event-loop runtime once pci-bus has to
 * ORIGINATE requests while serving — hot-plug is the case: staying resident is
 * the prerequisite for it (a RESCAN opcode over pci_scan_and_publish() is a small
 * addition), but a rescan has to publish to device-manager mid-loop, and knowing
 * WHEN to rescan needs the ACPI GPE/SCI path, which does not exist yet. */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"
#include "pci_bus_types.h"

/* Capability IDs in the PCI capability list (PCI 3.0 §6.7). */
#define PCI_CAP_ID_MSI 0x05u
#define PCI_CAP_ID_MSIX 0x11u

/* Config-space registers we touch beyond enumeration. */
#define PCI_REG_COMMAND 0x04u
#define PCI_REG_CAP_PTR 0x34u
#define PCI_REG_BAR0 0x10u

#define PCI_CMD_BUS_MASTER (1u << 2)
#define PCI_CMD_INTX_DISABLE (1u << 10)
#define PCI_STATUS_CAP_LIST (1u << 4) /* status is the high half of reg 0x04 */

/* MSI-X capability layout, relative to the capability offset. */
#define MSIX_CTRL_TABLE_SIZE_MASK 0x7FFu
#define MSIX_CTRL_FUNCTION_MASK (1u << 14)
#define MSIX_CTRL_ENABLE (1u << 15)
#define MSIX_REG_TABLE 0x04u
#define MSIX_TABLE_BIR_MASK 0x7u
#define MSIX_ENTRY_BYTES 16u
#define MSIX_ENTRY_VECTOR_CONTROL 0x0Cu
#define MSIX_VECTOR_CONTROL_MASKED 1u

/* MSI capability layout, relative to the capability offset. */
#define MSI_CTRL_ENABLE (1u << 0)
#define MSI_CTRL_MULTI_MSG_ENABLE_MASK (7u << 4)
#define MSI_CTRL_64BIT (1u << 7)
#define MSI_REG_ADDRESS_LO 0x04u

/* Devices whose interrupts this service has programmed. Small: only bus-master
 * devices with a real driver ever appear here. */
#define MSI_BINDING_MAX 8u

typedef struct {
    uint8_t in_use;
    uint8_t kind; /* WASMOS_PCI_MSI_KIND_* */
    uint16_t bdf;
    uint8_t cap_offset;
    int32_t owner_endpoint; /* the driver that claimed this function */
    uint32_t bound_mask;    /* entries currently unmasked */
} msi_binding_t;

static msi_binding_t g_msi_bindings[MSI_BINDING_MAX];

/* Log one record to serial for debug visibility. */
static void log_record(const pci_device_record_t* rec) {
    if (!rec) {
        return;
    }
    (void)printf(
        "[pci-bus] dev %02X:%02X.%02X class %02X:%02X:%02X vid:did %04X:%04X mmio %02X irq %02X\n",
        (unsigned)rec->bus, (unsigned)rec->device, (unsigned)rec->function,
        (unsigned)rec->class_code, (unsigned)rec->subclass, (unsigned)rec->prog_if,
        (unsigned)rec->vendor_id, (unsigned)rec->device_id, (unsigned)rec->mmio_hint,
        (unsigned)rec->irq_hint);
}

/* Read a 32-bit register from PCI config space using mechanism 1.
 * Bit 31 of the address register is the enable bit. */
static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t reg) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) | ((uint32_t)reg & 0xFCu);
    (void)wasmos_io_out32(PCI_CFG_ADDR_PORT, (int32_t)address);
    return (uint32_t)wasmos_io_in32(PCI_CFG_DATA_PORT);
}

static void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t reg,
                               uint32_t value) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) | ((uint32_t)reg & 0xFCu);
    (void)wasmos_io_out32(PCI_CFG_ADDR_PORT, (int32_t)address);
    (void)wasmos_io_out32(PCI_CFG_DATA_PORT, (int32_t)value);
}

/* bdf packs the routing id the way the IPC opcodes carry it. */
static uint8_t bdf_bus(uint16_t bdf) {
    return (uint8_t)(bdf >> 8);
}
static uint8_t bdf_device(uint16_t bdf) {
    return (uint8_t)((bdf >> 3) & 0x1Fu);
}
static uint8_t bdf_function(uint16_t bdf) {
    return (uint8_t)(bdf & 0x7u);
}

static uint32_t cfg_read(uint16_t bdf, uint8_t reg) {
    return pci_config_read32(bdf_bus(bdf), bdf_device(bdf), bdf_function(bdf), reg);
}

static void cfg_write(uint16_t bdf, uint8_t reg, uint32_t value) {
    pci_config_write32(bdf_bus(bdf), bdf_device(bdf), bdf_function(bdf), reg, value);
}

static int bdf_present(uint16_t bdf) {
    return (cfg_read(bdf, 0x00u) & 0xFFFFu) != 0xFFFFu;
}

/* Walk the capability list for `cap_id`; returns its config-space offset, or 0.
 * The loop is bounded because a malformed list can point at itself. */
static uint8_t pci_cap_find(uint16_t bdf, uint8_t cap_id) {
    uint32_t cmd_status = cfg_read(bdf, PCI_REG_COMMAND);
    if (((cmd_status >> 16) & PCI_STATUS_CAP_LIST) == 0u) {
        return 0;
    }
    uint8_t offset = (uint8_t)(cfg_read(bdf, PCI_REG_CAP_PTR) & 0xFCu);
    for (uint32_t hops = 0; hops < 48u && offset >= 0x40u; ++hops) {
        uint32_t header = cfg_read(bdf, offset);
        if ((header & 0xFFu) == cap_id) {
            return offset;
        }
        offset = (uint8_t)((header >> 8) & 0xFCu);
    }
    return 0;
}

/* Message Control is the high half of the capability's first dword. */
static uint16_t cap_control_read(uint16_t bdf, uint8_t cap) {
    return (uint16_t)((cfg_read(bdf, cap) >> 16) & 0xFFFFu);
}

static void cap_control_write(uint16_t bdf, uint8_t cap, uint16_t control) {
    uint32_t dword = cfg_read(bdf, cap);
    cfg_write(bdf, cap, (dword & 0x0000FFFFu) | ((uint32_t)control << 16));
}

/* Physical base of a BAR, resolving the 64-bit form. Returns 0 if the BAR is an
 * I/O BAR or unassigned — neither can hold an MSI-X table. */
static uint64_t pci_bar_phys(uint16_t bdf, uint8_t bir) {
    if (bir > 5u) {
        return 0;
    }
    uint8_t reg = (uint8_t)(PCI_REG_BAR0 + 4u * bir);
    uint32_t bar = cfg_read(bdf, reg);
    if ((bar & 1u) != 0u) {
        return 0; /* I/O space */
    }
    uint64_t base = (uint64_t)(bar & 0xFFFFFFF0u);
    if (((bar >> 1) & 0x3u) == 0x2u) { /* 64-bit memory BAR */
        if (bir >= 5u) {
            return 0;
        }
        base |= (uint64_t)cfg_read(bdf, (uint8_t)(reg + 4u)) << 32;
    }
    return base;
}

/* Physical address of MSI-X table entry `entry`. */
static uint64_t msix_entry_phys(uint16_t bdf, uint8_t cap, uint32_t entry) {
    uint32_t table = cfg_read(bdf, (uint8_t)(cap + MSIX_REG_TABLE));
    uint64_t bar = pci_bar_phys(bdf, (uint8_t)(table & MSIX_TABLE_BIR_MASK));
    if (bar == 0) {
        return 0;
    }
    return bar + (uint64_t)(table & ~MSIX_TABLE_BIR_MASK) + (uint64_t)entry * MSIX_ENTRY_BYTES;
}

static int32_t mmio_write32(uint64_t phys, uint32_t value) {
    return wasmos_mmio_write32((int32_t)(uint32_t)phys, (int32_t)(uint32_t)(phys >> 32),
                               (int32_t)value);
}

/* ------------------------------------------------------------------ bindings */

static msi_binding_t* binding_find(uint16_t bdf) {
    for (uint32_t i = 0; i < MSI_BINDING_MAX; ++i) {
        if (g_msi_bindings[i].in_use && g_msi_bindings[i].bdf == bdf) {
            return &g_msi_bindings[i];
        }
    }
    return 0;
}

static msi_binding_t* binding_claim(uint16_t bdf, uint8_t kind, uint8_t cap, int32_t owner) {
    for (uint32_t i = 0; i < MSI_BINDING_MAX; ++i) {
        if (!g_msi_bindings[i].in_use) {
            g_msi_bindings[i].in_use = 1;
            g_msi_bindings[i].bdf = bdf;
            g_msi_bindings[i].kind = kind;
            g_msi_bindings[i].cap_offset = cap;
            g_msi_bindings[i].owner_endpoint = owner;
            g_msi_bindings[i].bound_mask = 0;
            return &g_msi_bindings[i];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ MSI query */

/* Report what the function supports, preferring MSI-X: it has per-vector
 * addressing and per-entry masking, where plain MSI needs one contiguous
 * naturally-aligned vector block that the kernel's allocator does not hand out.
 * That is why MSI is reported as exactly one vector. */
static int32_t msi_query(uint16_t bdf, uint32_t* out_kind, uint32_t* out_vectors) {
    if (!bdf_present(bdf)) {
        return WASMOS_ERR_MSI_BAD_DEVICE;
    }
    uint8_t cap = pci_cap_find(bdf, PCI_CAP_ID_MSIX);
    if (cap != 0) {
        *out_kind = WASMOS_PCI_MSI_KIND_MSIX;
        *out_vectors = (cap_control_read(bdf, cap) & MSIX_CTRL_TABLE_SIZE_MASK) + 1u;
        return 0;
    }
    cap = pci_cap_find(bdf, PCI_CAP_ID_MSI);
    if (cap != 0) {
        *out_kind = WASMOS_PCI_MSI_KIND_MSI;
        /* TODO: multi-message MSI needs a contiguous aligned vector block from
         * the kernel (an msi_alloc_block); until then one message per device. */
        *out_vectors = 1u;
        return 0;
    }
    return WASMOS_ERR_MSI_NO_CAPABILITY;
}

/* ------------------------------------------------------------------- MSI bind */

static int32_t msix_bind(msi_binding_t* b, uint32_t entry, uint32_t address_lo, uint32_t address_hi,
                         uint32_t data) {
    uint16_t control = cap_control_read(b->bdf, b->cap_offset);
    if (entry > (control & MSIX_CTRL_TABLE_SIZE_MASK)) {
        return WASMOS_ERR_MSI_BAD_ENTRY;
    }
    uint64_t phys = msix_entry_phys(b->bdf, b->cap_offset, entry);
    if (phys == 0) {
        return WASMOS_ERR_MSI_MAP_FAILED;
    }
    /* Address and data first, vector control last: clearing the mask bit is what
     * arms the entry, so it must not be armed while it still points anywhere
     * else. */
    if (mmio_write32(phys + 0u, address_lo) != 0 || mmio_write32(phys + 4u, address_hi) != 0 ||
        mmio_write32(phys + 8u, data) != 0 ||
        mmio_write32(phys + MSIX_ENTRY_VECTOR_CONTROL, 0u) != 0) {
        return WASMOS_ERR_MSI_MAP_FAILED;
    }
    /* Enable the capability and lift the function-wide mask. */
    control = (uint16_t)((control | MSIX_CTRL_ENABLE) & ~(uint16_t)MSIX_CTRL_FUNCTION_MASK);
    cap_control_write(b->bdf, b->cap_offset, control);
    return 0;
}

static int32_t msi_bind_legacy(msi_binding_t* b, uint32_t entry, uint32_t address_lo,
                               uint32_t address_hi, uint32_t data) {
    if (entry != 0u) {
        return WASMOS_ERR_MSI_BAD_ENTRY;
    }
    uint16_t control = cap_control_read(b->bdf, b->cap_offset);
    uint8_t cap = b->cap_offset;
    cfg_write(b->bdf, (uint8_t)(cap + MSI_REG_ADDRESS_LO), address_lo);
    if ((control & MSI_CTRL_64BIT) != 0u) {
        cfg_write(b->bdf, (uint8_t)(cap + 0x08u), address_hi);
        cfg_write(b->bdf, (uint8_t)(cap + 0x0Cu), data & 0xFFFFu);
    } else {
        cfg_write(b->bdf, (uint8_t)(cap + 0x08u), data & 0xFFFFu);
    }
    /* One message: multiple-message-enable stays 0. */
    control = (uint16_t)((control & ~(uint16_t)MSI_CTRL_MULTI_MSG_ENABLE_MASK) | MSI_CTRL_ENABLE);
    cap_control_write(b->bdf, cap, control);
    return 0;
}

static int32_t msi_bind(uint16_t bdf, uint32_t entry, uint32_t address_lo, uint32_t address_hi,
                        uint32_t data, int32_t requester) {
    if (!bdf_present(bdf)) {
        return WASMOS_ERR_MSI_BAD_DEVICE;
    }
    msi_binding_t* b = binding_find(bdf);
    if (b && b->owner_endpoint != requester) {
        /* One driver per function. Without this a driver could redirect another
         * device's interrupts to its own endpoint. */
        return WASMOS_ERR_MSI_NOT_DEVICE_OWNER;
    }
    if (!b) {
        uint32_t kind = 0;
        uint32_t vectors = 0;
        int32_t rc = msi_query(bdf, &kind, &vectors);
        if (rc != 0) {
            return rc;
        }
        uint8_t cap = pci_cap_find(bdf, (kind == WASMOS_PCI_MSI_KIND_MSIX) ? PCI_CAP_ID_MSIX
                                                                           : PCI_CAP_ID_MSI);
        b = binding_claim(bdf, (uint8_t)kind, cap, requester);
        if (!b) {
            return WASMOS_ERR_MSI_NO_CAPABILITY;
        }
    }
    if (entry >= 32u) {
        return WASMOS_ERR_MSI_BAD_ENTRY;
    }

    int32_t rc = (b->kind == WASMOS_PCI_MSI_KIND_MSIX)
                     ? msix_bind(b, entry, address_lo, address_hi, data)
                     : msi_bind_legacy(b, entry, address_lo, address_hi, data);
    if (rc != 0) {
        return rc;
    }
    b->bound_mask |= (1u << entry);

    /* Bus mastering must be on for the device's message write to reach the
     * interrupt controller at all, and INTx must be off or the device keeps
     * asserting its shared wire alongside the messages. Writing the status half
     * as zero leaves its write-1-to-clear bits alone. */
    uint32_t cmd = cfg_read(bdf, PCI_REG_COMMAND) & 0xFFFFu;
    cfg_write(bdf, PCI_REG_COMMAND, cmd | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE);
    return 0;
}

static int32_t msi_unbind(uint16_t bdf, uint32_t entry, int32_t requester) {
    msi_binding_t* b = binding_find(bdf);
    if (!b) {
        return WASMOS_ERR_MSI_BAD_DEVICE;
    }
    if (b->owner_endpoint != requester) {
        return WASMOS_ERR_MSI_NOT_DEVICE_OWNER;
    }
    if (entry >= 32u || (b->bound_mask & (1u << entry)) == 0u) {
        return WASMOS_ERR_MSI_BAD_ENTRY;
    }

    if (b->kind == WASMOS_PCI_MSI_KIND_MSIX) {
        uint64_t phys = msix_entry_phys(b->bdf, b->cap_offset, entry);
        if (phys == 0 ||
            mmio_write32(phys + MSIX_ENTRY_VECTOR_CONTROL, MSIX_VECTOR_CONTROL_MASKED) != 0) {
            return WASMOS_ERR_MSI_MAP_FAILED;
        }
    }
    b->bound_mask &= ~(1u << entry);

    if (b->bound_mask == 0u) {
        uint16_t control = cap_control_read(b->bdf, b->cap_offset);
        control = (uint16_t)(control &
                             ~(uint16_t)((b->kind == WASMOS_PCI_MSI_KIND_MSIX) ? MSIX_CTRL_ENABLE
                                                                               : MSI_CTRL_ENABLE));
        cap_control_write(b->bdf, b->cap_offset, control);
        b->in_use = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------- scanning */

/* Encode one PCI record into DEVMGR_PUBLISH_DEVICE IPC arguments and send.
 * Encoding:
 *   arg0 = (bus<<24) | (device<<16) | (function<<8) | class_code
 *   arg1 = (subclass<<24) | (prog_if<<16) | vendor_id
 *   arg2 = (io_port_base<<16) | device_id
 *   arg3 = (io_port_base<<16) | (irq_hint<<8) | mmio_hint */
static void publish_record(int32_t devmgr_endpoint, int32_t source_endpoint,
                           const pci_device_record_t* rec, int32_t request_id) {
    if (!rec) {
        return;
    }
    uint32_t arg0 = ((uint32_t)rec->bus << 24) | ((uint32_t)rec->device << 16) |
                    ((uint32_t)rec->function << 8) | (uint32_t)rec->class_code;
    uint32_t arg1 =
        ((uint32_t)rec->subclass << 24) | ((uint32_t)rec->prog_if << 16) | (uint32_t)rec->vendor_id;
    uint32_t arg2 = ((uint32_t)rec->io_port_base << 16) | (uint32_t)rec->device_id;
    uint32_t arg3 = ((uint32_t)rec->io_port_base << 16) | ((uint32_t)rec->irq_hint << 8) |
                    (uint32_t)rec->mmio_hint;
    (void)wasmos_ipc_send(devmgr_endpoint, source_endpoint, DEVMGR_PUBLISH_DEVICE, request_id,
                          (int32_t)arg0, (int32_t)arg1, (int32_t)arg2, (int32_t)arg3);
}

/* Brute-force scan (buses 0-255, devices 0-31, functions 0-7), publishing each
 * present function. Stops scanning functions for single-function devices
 * (header type bit 7 = 0). Returns the next free request id. */
static int32_t pci_scan_and_publish(int32_t devmgr_endpoint, int32_t source_endpoint,
                                    int32_t request_id) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint32_t id_reg = pci_config_read32((uint8_t)bus, device, function, 0x00);
                uint16_t vendor_id = (uint16_t)(id_reg & 0xFFFFu);
                if (vendor_id == 0xFFFFu) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                pci_device_record_t rec;
                rec.bus = (uint8_t)bus;
                rec.device = device;
                rec.function = function;
                rec.vendor_id = vendor_id;
                rec.device_id = (uint16_t)((id_reg >> 16) & 0xFFFFu);
                uint32_t class_reg = pci_config_read32((uint8_t)bus, device, function, 0x08);
                rec.class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                rec.subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                rec.prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);
                uint32_t bar0 = pci_config_read32((uint8_t)bus, device, function, 0x10);
                rec.mmio_hint = ((bar0 & 0x1u) == 0u && (bar0 & 0xFFFFFFF0u) != 0u) ? 1u : 0u;
                rec.io_port_base = ((bar0 & 0x1u) != 0u) ? (uint16_t)(bar0 & 0xFFFCu) : 0u;
                uint32_t irq_reg = pci_config_read32((uint8_t)bus, device, function, 0x3C);
                rec.irq_hint = (uint8_t)(irq_reg & 0xFFu);
                /* PCI INTx is level-triggered, active-low (PCI spec). If this
                 * device drives an interrupt pin (0x3D != 0), mark its line in
                 * the IOAPIC accordingly — the boot default is active-high,
                 * which makes PCI INTx re-deliver only once. A device whose
                 * driver later moves to MSI stops using the line entirely. */
                uint8_t irq_pin = (uint8_t)((irq_reg >> 8) & 0xFFu);
                if (irq_pin != 0u && rec.irq_hint != 0u && rec.irq_hint < 16u) {
                    (void)wasmos_irq_configure((int32_t)rec.irq_hint,
                                               WASMOS_IRQ_TRIGGER_LEVEL | WASMOS_IRQ_POLARITY_LOW);
                }
                log_record(&rec);
                publish_record(devmgr_endpoint, source_endpoint, &rec, request_id++);
                uint32_t header_reg = pci_config_read32((uint8_t)bus, device, 0, 0x0C);
                if (function == 0 && (((header_reg >> 16) & 0x80u) == 0)) {
                    break;
                }
            }
        }
    }
    return request_id;
}

/* ----------------------------------------------------------------- event loop */

static void reply_error(int32_t dest, int32_t src, int32_t request_id, int32_t code) {
    (void)wasmos_ipc_send(dest, src, PCI_IPC_ERROR, request_id, 0, code, 0, 0);
}

static void handle_request(int32_t service_endpoint, const wasmos_ipc_message_t* msg) {
    int32_t reply_to = msg->source;
    if (reply_to < 0) {
        return;
    }
    uint32_t arg0 = (uint32_t)msg->arg0;

    switch (msg->type) {
    case PCI_IPC_MSI_QUERY: {
        uint32_t kind = 0;
        uint32_t vectors = 0;
        int32_t rc = msi_query((uint16_t)(arg0 & 0xFFFFu), &kind, &vectors);
        if (rc != 0) {
            reply_error(reply_to, service_endpoint, msg->request_id, rc);
            return;
        }
        (void)wasmos_ipc_send(reply_to, service_endpoint, PCI_IPC_RESP, msg->request_id,
                              (int32_t)kind, (int32_t)vectors, 0, 0);
        return;
    }
    case PCI_IPC_MSI_BIND: {
        uint16_t bdf = (uint16_t)((arg0 >> 8) & 0xFFFFu);
        uint32_t entry = arg0 & 0xFFu;
        int32_t rc = msi_bind(bdf, entry, (uint32_t)msg->arg1, (uint32_t)msg->arg2,
                              (uint32_t)msg->arg3, reply_to);
        if (rc != 0) {
            reply_error(reply_to, service_endpoint, msg->request_id, rc);
            return;
        }
        (void)wasmos_ipc_send(reply_to, service_endpoint, PCI_IPC_RESP, msg->request_id,
                              (int32_t)entry, 0, 0, 0);
        return;
    }
    case PCI_IPC_MSI_UNBIND: {
        uint16_t bdf = (uint16_t)((arg0 >> 8) & 0xFFFFu);
        uint32_t entry = arg0 & 0xFFu;
        int32_t rc = msi_unbind(bdf, entry, reply_to);
        if (rc != 0) {
            reply_error(reply_to, service_endpoint, msg->request_id, rc);
            return;
        }
        (void)wasmos_ipc_send(reply_to, service_endpoint, PCI_IPC_RESP, msg->request_id,
                              (int32_t)entry, 0, 0, 0);
        return;
    }
    default:
        /* Unknown or unsolicited: stay silent. Replying to a message nobody
         * asked about is how two services end up ping-ponging errors. */
        return;
    }
}

/* Service entry point. Looks up "devmgr.inv", registers "pci" so drivers can
 * find it, scans and publishes, sends DEVMGR_PCI_SCAN_DONE, announces readiness,
 * and then serves MSI programming requests for the rest of the boot. */
WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t ignored_arg1,
                                      int32_t ignored_arg2, int32_t ignored_arg3) {
    /* proc.endpoint now comes from the spawn-info contract, not an entry arg. */
    proc_endpoint = wasmos_startup_proc_endpoint();
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;
    if (proc_endpoint < 0) {
        return -1;
    }

    int32_t source_endpoint = wasmos_ipc_create_endpoint();
    if (source_endpoint < 0) {
        return -1;
    }
    /* Requests land on their own endpoint: a reply-matching loop on
     * source_endpoint discards messages it did not ask for, which is exactly how
     * a driver's bind request would go missing during a lookup or a ready ack. */
    int32_t service_endpoint = wasmos_ipc_create_endpoint();
    if (service_endpoint < 0) {
        return -1;
    }
    int32_t devmgr_endpoint =
        wasmos_sys_svc_lookup_retry(proc_endpoint, source_endpoint, "devmgr.inv", 1, 1024);
    if (devmgr_endpoint == -1) {
        return -1;
    }
    /* Register before scanning: publishing devices is what makes device-manager
     * spawn drivers, and a driver must never find the bus service missing. */
    if (wasmos_svc_register(proc_endpoint, service_endpoint, "pci", 1) < 0) {
        (void)printf("[pci-bus] service registration failed\n");
    }

    int32_t request_id = pci_scan_and_publish(devmgr_endpoint, source_endpoint, 1);
    (void)wasmos_ipc_send(devmgr_endpoint, source_endpoint, DEVMGR_PCI_SCAN_DONE, request_id, 0, 0,
                          0, 0);
    wasmos_sys_notify_ready(proc_endpoint, source_endpoint);

    for (;;) {
        int32_t rc = wasmos_ipc_select_one(service_endpoint);
        if (rc < 0) {
            continue;
        }
        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);
        handle_request(service_endpoint, &msg);
    }
}
