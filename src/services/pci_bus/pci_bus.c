/* pci_bus.c - WASM service: owns PCI configuration space.
 *
 * Two jobs. It enumerates config space and publishes every function to
 * device-manager (DEVMGR_PUBLISH_DEVICE_DESC), and it then STAYS RESIDENT serving
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

/* Descriptor slots in the publish buffer. Matches device-manager's registry cap;
 * a slot per device is what lets publishes be fire-and-forget without the
 * publisher overwriting a descriptor the receiver has not read yet. */
#define PCI_MAX_PUBLISHED_DEVICES 64u

/* Config-space registers accessed beyond enumeration. */
#define PCI_REG_COMMAND 0x04u
#define PCI_REG_CAP_PTR 0x34u
#define PCI_REG_BAR0 0x10u

#define PCI_CMD_IO_SPACE (1u << 0)
#define PCI_CMD_MEM_SPACE (1u << 1)
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

/* Log one function, including where its registers live and which interrupt
 * capabilities it has -- the facts a driver author needs and cannot obtain
 * otherwise, since no driver is allowed to read config space. */
static void log_desc(const wasmos_pci_device_desc_t* d) {
    if (!d) {
        return;
    }
    (void)printf("[pci-bus] %02X:%02X.%X class %02X:%02X:%02X %04X:%04X irq %02X pin %u%s%s\n",
                 (unsigned)d->bus,
                 (unsigned)d->device,
                 (unsigned)d->function,
                 (unsigned)d->class_code,
                 (unsigned)d->subclass,
                 (unsigned)d->prog_if,
                 (unsigned)d->vendor_id,
                 (unsigned)d->device_id,
                 (unsigned)d->irq_line,
                 (unsigned)d->irq_pin,
                 d->msix_cap_offset ? " msix" : "",
                 (!d->msix_cap_offset && d->msi_cap_offset) ? " msi" : "");
    for (uint32_t i = 0; i < WASMOS_PCI_BAR_COUNT; ++i) {
        if (d->bars[i].kind == WASMOS_PCI_BAR_NONE) {
            continue;
        }
        (void)printf("[pci-bus]   bar%u %s %08X size %u%s\n",
                     (unsigned)i,
                     (d->bars[i].kind == WASMOS_PCI_BAR_IO)
                         ? "io "
                         : ((d->bars[i].kind == WASMOS_PCI_BAR_MEM64) ? "m64" : "m32"),
                     (unsigned)(d->bars[i].base & 0xFFFFFFFFu),
                     (unsigned)(d->bars[i].size & 0xFFFFFFFFu),
                     d->bars[i].prefetchable ? " pf" : "");
    }
}

/* Read a 32-bit register from PCI config space using mechanism 1.
 * Bit 31 of the address register is the enable bit. */
static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t reg) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) | ((uint32_t)reg & 0xFCu);
    (void)wasmos_io_out32(PCI_CFG_ADDR_PORT, (int32_t)address);
    uint32_t value = 0xFFFFFFFFu; /* an absent device reads back all-ones */
    (void)wasmos_io_in32(PCI_CFG_DATA_PORT, &value);
    return value;
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
    return wasmos_mmio_write32(
        (int32_t)(uint32_t)phys, (int32_t)(uint32_t)(phys >> 32), (int32_t)value);
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
        uint8_t cap = pci_cap_find(
            bdf, (kind == WASMOS_PCI_MSI_KIND_MSIX) ? PCI_CAP_ID_MSIX : PCI_CAP_ID_MSI);
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

/* Decode one BAR into the descriptor. A 64-bit memory BAR consumes the NEXT
 * slot for its high half, so the caller is told to skip it -- six registers is
 * not always six regions.
 *
 * Sizing means writing all-ones and reading back the mask, which requires the
 * function to stop decoding that space first. Decode is enabled per space --
 * command bit 0 covers I/O, bit 1 covers memory -- so each probe quiesces only
 * the space it is measuring and cannot disturb the other.
 *
 * The one window that must not be quiesced is whatever backs the live console.
 * The kernel takes the UEFI GOP framebuffer long before this scan runs, and that
 * framebuffer IS a memory BAR of the VGA function (the boot log shows
 * [framebuffer] init 0x80000000 and bar0 m32 80000000 on 00:02.0 -- one address,
 * two views). Turning its decode off would not make the display flicker, since
 * scanout reads the device's memory internally rather than through the address
 * decoder; it would silently DROP any CPU write landing in the probe window,
 * leaving stale pixels until something redrew. Cheap to avoid, so avoid it. */
/* Size an I/O BAR: quiesce I/O decode, write all-ones, read back the mask, then
 * restore both the BAR and the command register. The mask's lowest set bit is
 * the region length. Memory decode is left alone throughout (see pci_decode_bar).
 * Returns 0 when the device does not implement the BAR. */
static uint64_t pci_probe_bar_size(uint16_t bdf, uint8_t reg, uint32_t original,
                                   uint32_t decode_bit, uint32_t addr_mask) {
    uint32_t cmd = cfg_read(bdf, PCI_REG_COMMAND) & 0xFFFFu;
    uint32_t mask = 0;

    cfg_write(bdf, PCI_REG_COMMAND, cmd & ~decode_bit);
    cfg_write(bdf, reg, 0xFFFFFFFFu);
    mask = cfg_read(bdf, reg) & addr_mask;
    cfg_write(bdf, reg, original);
    cfg_write(bdf, PCI_REG_COMMAND, cmd);

    if (mask == 0u) {
        return 0;
    }
    /* Length is the value of the lowest set bit: the mask's clear low bits are
     * the offset bits the device decodes. */
    return (uint64_t)(((~mask) + 1u) & 0xFFFFFFFFu);
}

/* Physical base/size of the framebuffer the kernel is scanning out of, or 0.
 * Queried once; a service cannot see boot_info directly. */
static uint64_t g_console_fb_base;
static uint64_t g_console_fb_size;

static void pci_load_console_framebuffer(void) {
    wasmos_framebuffer_info_t info;
    if (wasmos_framebuffer_info(&info, (int32_t)sizeof(info)) != 0) {
        return;
    }
    g_console_fb_base = info.framebuffer_base;
    g_console_fb_size = info.framebuffer_size;
}

/* True when this memory BAR plausibly backs the live console, whose decode must
 * stay on. The test has to be a heuristic because the BAR's length is exactly
 * what the probe would report -- so it asks whether the framebuffer starts at or
 * after this base, within a window larger than any framebuffer BAR. It errs
 * toward skipping: a false positive costs one unknown BAR size, a false negative
 * drops console writes. */
static int pci_bar_is_console(uint64_t base) {
    if (g_console_fb_base == 0u || base == 0u) {
        return 0;
    }
    return (g_console_fb_base >= base && g_console_fb_base < base + 0x10000000ull) ? 1 : 0;
}

static uint32_t pci_decode_bar(uint16_t bdf, uint32_t index, wasmos_pci_bar_t* out) {
    uint8_t reg = (uint8_t)(PCI_REG_BAR0 + 4u * index);
    uint32_t value = cfg_read(bdf, reg);
    uint32_t consumed = 1u;

    out->kind = WASMOS_PCI_BAR_NONE;
    out->prefetchable = 0;
    out->reserved0 = 0;
    out->reserved1 = 0;
    out->base = 0;
    out->size = 0;
    if (value == 0u) {
        return consumed;
    }
    if ((value & 1u) != 0u) {
        out->kind = WASMOS_PCI_BAR_IO;
        out->base = (uint64_t)(value & 0xFFFFFFFCu);
        out->size = pci_probe_bar_size(bdf, reg, value, PCI_CMD_IO_SPACE, 0xFFFFFFFCu);
        return consumed;
    }
    out->prefetchable = ((value & 0x8u) != 0u) ? 1u : 0u;
    out->base = (uint64_t)(value & 0xFFFFFFF0u);
    if (((value >> 1) & 0x3u) == 0x2u) {
        out->kind = WASMOS_PCI_BAR_MEM64;
        if (index + 1u < WASMOS_PCI_BAR_COUNT) {
            out->base |= (uint64_t)cfg_read(bdf, (uint8_t)(reg + 4u)) << 32;
            consumed = 2u;
        }
    } else {
        out->kind = WASMOS_PCI_BAR_MEM32;
    }
    if (!pci_bar_is_console(out->base)) {
        out->size = pci_probe_bar_size(bdf, reg, value, PCI_CMD_MEM_SPACE, 0xFFFFFFF0u);
    }
    return consumed;
}

/* Fill the descriptor for one function: identity, class triplet, interrupt
 * routing, every BAR, and where the interrupt capabilities live. Only pci-bus
 * can walk the capability list, so it reports the offsets rather than making
 * each consumer ask for them. */
static void pci_fill_desc(uint16_t bdf, wasmos_pci_device_desc_t* desc) {
    uint32_t id_reg = cfg_read(bdf, 0x00u);
    uint32_t class_reg = cfg_read(bdf, 0x08u);
    uint32_t irq_reg = cfg_read(bdf, 0x3Cu);

    __builtin_memset(desc, 0, sizeof(*desc));
    desc->version = WASMOS_PCI_DEVICE_DESC_VERSION;
    desc->bus = bdf_bus(bdf);
    desc->device = bdf_device(bdf);
    desc->function = bdf_function(bdf);
    desc->vendor_id = (uint16_t)(id_reg & 0xFFFFu);
    desc->device_id = (uint16_t)((id_reg >> 16) & 0xFFFFu);
    desc->class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
    desc->subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
    desc->prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);
    desc->irq_line = (uint8_t)(irq_reg & 0xFFu);
    desc->irq_pin = (uint8_t)((irq_reg >> 8) & 0xFFu);
    desc->msi_cap_offset = pci_cap_find(bdf, PCI_CAP_ID_MSI);
    desc->msix_cap_offset = pci_cap_find(bdf, PCI_CAP_ID_MSIX);

    for (uint32_t i = 0; i < WASMOS_PCI_BAR_COUNT;) {
        i += pci_decode_bar(bdf, i, &desc->bars[i]);
    }
}

/* Publish one function. Each device gets its own slot in the shared buffer, so
 * the publisher never overwrites a descriptor the receiver has not read yet and
 * no per-device acknowledgement is needed. */
static void publish_desc(int32_t devmgr_endpoint, int32_t source_endpoint, int32_t buffer_id,
                         uint32_t slot, const wasmos_pci_device_desc_t* desc, int32_t request_id) {
    uint32_t offset = slot * (uint32_t)sizeof(*desc);
    if (wasmos_xfer_buffer_write(
            buffer_id, addr_cast(int32_t, desc), (int32_t)sizeof(*desc), (int32_t)offset) != 0) {
        (void)printf("[pci-bus] descriptor write failed slot=%u\n", (unsigned)slot);
        return;
    }
    (void)wasmos_ipc_send(devmgr_endpoint,
                          source_endpoint,
                          DEVMGR_PUBLISH_DEVICE_DESC,
                          request_id,
                          buffer_id,
                          (int32_t)offset,
                          (int32_t)sizeof(*desc),
                          0);
}

/* Brute-force scan (buses 0-255, devices 0-31, functions 0-7), publishing each
 * present function. Stops scanning functions for single-function devices
 * (header type bit 7 = 0), and abandons the scan once PCI_MAX_PUBLISHED_DEVICES
 * slots are filled — later functions are then never published. Returns the next
 * free request id. */
static int32_t pci_scan_and_publish(int32_t devmgr_endpoint, int32_t source_endpoint,
                                    int32_t buffer_id, int32_t request_id) {
    uint32_t slot = 0;
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint16_t bdf =
                    (uint16_t)(((uint32_t)bus << 8) | ((uint32_t)device << 3) | (uint32_t)function);
                wasmos_pci_device_desc_t desc;
                if (!bdf_present(bdf)) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                if (slot >= PCI_MAX_PUBLISHED_DEVICES) {
                    (void)printf("[pci-bus] device table full; %u published\n", (unsigned)slot);
                    return request_id;
                }
                pci_fill_desc(bdf, &desc);
                /* PCI INTx is level-triggered, active-low (PCI spec). If this
                 * device drives an interrupt pin (0x3D != 0), mark its line in
                 * the IOAPIC accordingly — the boot default is active-high,
                 * which makes PCI INTx re-deliver only once. A device whose
                 * driver later moves to MSI stops using the line entirely. */
                if (desc.irq_pin != 0u && desc.irq_line != 0u && desc.irq_line < 16u) {
                    (void)wasmos_irq_configure((int32_t)desc.irq_line,
                                               WASMOS_IRQ_TRIGGER_LEVEL | WASMOS_IRQ_POLARITY_LOW);
                }
                log_desc(&desc);
                publish_desc(
                    devmgr_endpoint, source_endpoint, buffer_id, slot, &desc, request_id++);
                slot++;
                if (function == 0 && ((cfg_read(bdf, 0x0Cu) >> 16) & 0x80u) == 0u) {
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

/* Serve one request on the service endpoint.
 *
 * Argument layout, shared with every driver that programs MSI/MSI-X.  `bdf` is
 * the standard 16-bit PCI address (bus << 8) | (slot << 3) | function, and
 * `entry` is the MSI-X table index (0 for plain MSI):
 *
 *   PCI_IPC_MSI_QUERY   arg0 = bdf (low 16 bits; no entry field)
 *                       reply arg0 = WASMOS_PCI_MSI_KIND_* , arg1 = vector count
 *   PCI_IPC_MSI_BIND    arg0 = (bdf << 8) | entry  [entry in bits 7:0]
 *                       arg1 = MSI address low, arg2 = address high,
 *                       arg3 = message data — the triple the kernel returned
 *                       from wasmos_msi_alloc, written verbatim into the device
 *                       reply arg0 = entry
 *   PCI_IPC_MSI_UNBIND  arg0 = (bdf << 8) | entry
 *                       reply arg0 = entry
 *
 * A failure answers PCI_IPC_ERROR with the packed code in arg1 (arg0 is 0).  A
 * message from a negative source, and any opcode not listed above, is dropped
 * without a reply on purpose: answering an unsolicited message is how two
 * services end up trading errors forever.  Serves entirely out of PCI config
 * space, so it never blocks on another service. */
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
        (void)wasmos_ipc_send(reply_to,
                              service_endpoint,
                              PCI_IPC_RESP,
                              msg->request_id,
                              (int32_t)kind,
                              (int32_t)vectors,
                              0,
                              0);
        return;
    }
    case PCI_IPC_MSI_BIND: {
        uint16_t bdf = (uint16_t)((arg0 >> 8) & 0xFFFFu);
        uint32_t entry = arg0 & 0xFFu;
        int32_t rc = msi_bind(
            bdf, entry, (uint32_t)msg->arg1, (uint32_t)msg->arg2, (uint32_t)msg->arg3, reply_to);
        if (rc != 0) {
            reply_error(reply_to, service_endpoint, msg->request_id, rc);
            return;
        }
        (void)wasmos_ipc_send(
            reply_to, service_endpoint, PCI_IPC_RESP, msg->request_id, (int32_t)entry, 0, 0, 0);
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
        (void)wasmos_ipc_send(
            reply_to, service_endpoint, PCI_IPC_RESP, msg->request_id, (int32_t)entry, 0, 0, 0);
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
WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* The proc endpoint comes from the spawn-info contract; the entry args carry
     * nothing and the parameter is overwritten. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
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

    /* One buffer, one slot per device, borrowed to device-manager for the life
     * of the scan. Read-only: device-manager consumes descriptors, it never
     * writes them back. */
    int32_t desc_bid = wasmos_xfer_buffer_acquire(
        (int32_t)(PCI_MAX_PUBLISHED_DEVICES * sizeof(wasmos_pci_device_desc_t)));
    if (desc_bid < 0 ||
        wasmos_xfer_buffer_borrow(devmgr_endpoint, desc_bid, WASMOS_BUFFER_GRANT_READ) < 0) {
        (void)printf("[pci-bus] descriptor buffer unavailable\n");
        return -1;
    }

    pci_load_console_framebuffer();
    int32_t request_id = pci_scan_and_publish(devmgr_endpoint, source_endpoint, desc_bid, 1);
    (void)wasmos_ipc_send(
        devmgr_endpoint, source_endpoint, DEVMGR_PCI_SCAN_DONE, request_id, 0, 0, 0, 0);
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
