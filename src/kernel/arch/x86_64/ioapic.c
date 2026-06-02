#include "arch/x86_64/ioapic.h"
#include "irq.h"
#include "paging.h"
#include "serial.h"
#if WASMOS_SMP
#include "arch/x86_64/smp.h"
#include "arch/x86_64/lapic.h"
#endif

#include <stdint.h>

/*
 * I/O APIC driver (xAPIC mode).  Parses the ACPI MADT to discover the I/O APIC
 * physical base and any ISA IRQ source overrides, maps one 4 KB MMIO page into
 * kernel virtual space, then programs all 16 ISA Redirection Table Entries to
 * deliver IRQs at vectors IRQ_VECTOR_BASE+0 through IRQ_VECTOR_BASE+15 (32–47)
 * to LAPIC 0 (the BSP) in edge-triggered, active-high, initially masked mode.
 *
 * EOI path: ISA IRQs are edge-triggered, so only a LAPIC EOI is required after
 * each interrupt — the I/O APIC does not need a separate EOI write.
 *
 * This driver is compiled for all IRQ modes but only called (from
 * x86_irq_late_init) when WASMOS_IRQ_MODE == 2.
 */

/* I/O APIC default physical base (QEMU).  Overridden from MADT type-1. */
#define IOAPIC_PHYS_DEFAULT   0xFEC00000ULL

/* Reserved kernel VA at PT_A index 0xFF (255), physical 0xFF000 — BIOS ROM
 * region, guaranteed PFA-unreachable.  See LAPIC_VIRT_BASE rationale. */
#define IOAPIC_VIRT_BASE      0xFFFFFFFF800FF000ULL

/* Indirect MMIO register access. */
#define IOAPIC_REG_SELECT     0x00u
#define IOAPIC_REG_WINDOW     0x10u

/* Redirection Table Entry (RTE) register indices: 2 × 32-bit words per IRQ. */
#define IOAPIC_RTE_LO(n)      ((uint8_t)(0x10u + 2u * (n)))
#define IOAPIC_RTE_HI(n)      ((uint8_t)(0x11u + 2u * (n)))

/* RTE low-word field masks. */
#define IOAPIC_RTE_MASK_BIT   (1u << 16)
/*
 * ISA IRQs use level-triggered mode here even though the ISA bus default is
 * edge-triggered.  The reason is the mask-based dispatch model: after delivery
 * the kernel masks the RTE, dispatches an IPC to the driver, and unmasks only
 * after the driver reads the hardware register (irq_ack).  In edge-triggered
 * mode the IOAPIC silently discards any rising edge that arrives while the RTE
 * is masked — a new edge is required to re-arm delivery.  For PS/2 devices
 * (i8042 IRQ 1 / IRQ 12) the IRQ line stays asserted while OBF is set; when
 * the driver reads OBF the i8042 immediately loads the next queued byte back
 * into OBF, so the line never deasserts long enough for a new edge.  With
 * level-triggered mode, unmasking the RTE while the line is still HIGH causes
 * an immediate re-delivery, correctly draining back-to-back bytes.  The LAPIC
 * EOI also broadcasts an EOI signal (EOIS) back to the IOAPIC for
 * level-triggered vectors, clearing the Remote IRR automatically.
 */
#define IOAPIC_RTE_LEVEL      (1u << 15)
#define IOAPIC_RTE_ACTHI      (0u << 13)
#define IOAPIC_RTE_FIXED      (0u << 8)

/* Page table flags for MMIO mapping (mirrors lapic.c). */
#define PT_FLAG_PRESENT       (1ULL << 0)
#define PT_FLAG_WRITE         (1ULL << 1)
#define PT_FLAG_PCD           (1ULL << 4)
#define PT_FLAG_NX            (1ULL << 63)

static uintptr_t g_ioapic_base;
static uint8_t   g_gsi_map[16]; /* g_gsi_map[isa_irq] = gsi; default identity */

/* ------------------------------------------------------------------ MMIO I/O */

static inline void
ioapic_write_sel(uint8_t reg)
{
    *(volatile uint32_t *)(g_ioapic_base + IOAPIC_REG_SELECT) = reg;
}

static inline uint32_t
ioapic_read_win(void)
{
    return *(volatile uint32_t *)(g_ioapic_base + IOAPIC_REG_WINDOW);
}

static inline void
ioapic_write_win(uint32_t val)
{
    *(volatile uint32_t *)(g_ioapic_base + IOAPIC_REG_WINDOW) = val;
}

static inline uint32_t
ioapic_read_reg(uint8_t reg)
{
    ioapic_write_sel(reg);
    return ioapic_read_win();
}

static inline void
ioapic_write_reg(uint8_t reg, uint32_t val)
{
    ioapic_write_sel(reg);
    ioapic_write_win(val);
}

/* ---------------------------------------------------------- ACPI MADT structs */

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_hdr_t;

typedef struct {
    acpi_sdt_hdr_t hdr;
    uint32_t       lapic_phys;
    uint32_t       flags;
} __attribute__((packed)) acpi_madt_t;

typedef struct { uint8_t type; uint8_t len; } __attribute__((packed)) acpi_madt_entry_t;

typedef struct {            /* MADT entry type 1 — I/O APIC */
    acpi_madt_entry_t hdr;
    uint8_t           ioapic_id;
    uint8_t           reserved;
    uint32_t          ioapic_addr;
    uint32_t          gsi_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

typedef struct {            /* MADT entry type 2 — IRQ source override */
    acpi_madt_entry_t hdr;
    uint8_t           bus;
    uint8_t           irq_src;
    uint32_t          gsi;
    uint16_t          flags;
} __attribute__((packed)) acpi_madt_irq_override_t;

typedef struct {            /* MADT entry type 0 — Processor Local APIC */
    acpi_madt_entry_t hdr;
    uint8_t           processor_uid;
    uint8_t           apic_id;
    uint32_t          flags;           /* bit 0: LAPIC enabled; bit 1: online capable */
} __attribute__((packed)) acpi_madt_processor_t;

#define MADT_PROC_FLAG_ENABLED 0x1u

/* ---------------------------------------------------------------- MADT parser */

static uint64_t
find_xsdt_phys(const boot_info_t *boot_info)
{
    uint64_t xsdt;
    /* RSDP v2: xsdt_address at byte offset 24. */
    __builtin_memcpy(&xsdt, (const uint8_t *)boot_info->rsdp + 24, 8);
    return xsdt;
}

static void
madt_parse(uint64_t xsdt_phys, uint64_t *out_ioapic_phys)
{
    *out_ioapic_phys = IOAPIC_PHYS_DEFAULT;
    for (uint32_t i = 0; i < 16u; i++) {
        g_gsi_map[i] = (uint8_t)i;
    }

    acpi_sdt_hdr_t *xsdt       = (acpi_sdt_hdr_t *)(uintptr_t)xsdt_phys;
    uint32_t        entry_count = (xsdt->length - 36u) / 8u;
    const uint8_t  *entries     = (const uint8_t *)xsdt + 36;

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t tbl_phys;
        __builtin_memcpy(&tbl_phys, entries + i * 8u, 8);
        acpi_sdt_hdr_t *tbl = (acpi_sdt_hdr_t *)(uintptr_t)tbl_phys;
        if (__builtin_memcmp(tbl->signature, "APIC", 4) != 0) {
            continue;
        }

        const uint8_t *p   = (const uint8_t *)tbl + sizeof(acpi_madt_t);
        const uint8_t *end = (const uint8_t *)tbl + tbl->length;

#if WASMOS_SMP
        uint32_t bsp_apic_id = lapic_read_id();
#endif

        while (p < end) {
            const acpi_madt_entry_t *e = (const acpi_madt_entry_t *)p;
            if (e->len == 0) {
                break;
            }
            if (e->type == 0u) {
#if WASMOS_SMP
                const acpi_madt_processor_t *proc = (const acpi_madt_processor_t *)p;
                if ((proc->flags & MADT_PROC_FLAG_ENABLED) &&
                    proc->apic_id != (uint8_t)bsp_apic_id &&
                    g_cpu_count < WASMOS_MAX_CPUS) {
                    g_cpus[g_cpu_count].apic_id = proc->apic_id;
                    g_cpus[g_cpu_count].cpu_id  = g_cpu_count;
                    g_cpu_count++;
                }
#endif
            } else if (e->type == 1u) {
                const acpi_madt_ioapic_t *io = (const acpi_madt_ioapic_t *)p;
                *out_ioapic_phys = io->ioapic_addr;
            } else if (e->type == 2u) {
                const acpi_madt_irq_override_t *ov = (const acpi_madt_irq_override_t *)p;
                if (ov->irq_src < 16u) {
                    g_gsi_map[ov->irq_src] = (uint8_t)ov->gsi;
                }
            }
            p += e->len;
        }
        break;
    }
}

/* -------------------------------------------------------------- MMIO mapping */

static void
ioapic_map(uint64_t phys)
{
    uint64_t flags = PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_PCD | PT_FLAG_NX;
    int rc = paging_map_4k(IOAPIC_VIRT_BASE, phys, flags);
    if (rc != 0) {
        serial_write("[ioapic] mmio map failed\n");
        return;
    }
    g_ioapic_base = (uintptr_t)IOAPIC_VIRT_BASE;
}

/* --------------------------------------------------------- RTE initialisation */

static void
ioapic_program_rtes(void)
{
    for (uint32_t irq = 0; irq < 16u; irq++) {
        uint32_t vec = IRQ_VECTOR_BASE + irq;
        /* Start masked; drivers unmask via irq_unmask() after registration. */
        uint32_t lo = IOAPIC_RTE_MASK_BIT | IOAPIC_RTE_LEVEL |
                      IOAPIC_RTE_ACTHI    | IOAPIC_RTE_FIXED | vec;
        uint32_t hi = 0u; /* physical dest, LAPIC ID = 0 (BSP) */
        uint32_t gsi = g_gsi_map[irq];
        ioapic_write_reg(IOAPIC_RTE_HI(gsi), hi);
        ioapic_write_reg(IOAPIC_RTE_LO(gsi), lo);
    }
}

/* ----------------------------------------------------------------------- API */

void
ioapic_init(const boot_info_t *boot_info)
{
    uint64_t ioapic_phys;
    madt_parse(find_xsdt_phys(boot_info), &ioapic_phys);
    ioapic_map(ioapic_phys);
    if (!g_ioapic_base) {
        return;
    }
    ioapic_program_rtes();
    serial_write("[ioapic] init ok\n");
}

void
ioapic_mask_irq(uint32_t irq_line)
{
    if (irq_line >= 16u || !g_ioapic_base) {
        return;
    }
    uint32_t gsi = g_gsi_map[irq_line];
    uint32_t lo  = ioapic_read_reg(IOAPIC_RTE_LO(gsi));
    ioapic_write_reg(IOAPIC_RTE_LO(gsi), lo | IOAPIC_RTE_MASK_BIT);
}

void
ioapic_unmask_irq(uint32_t irq_line)
{
    if (irq_line >= 16u || !g_ioapic_base) {
        return;
    }
    uint32_t gsi = g_gsi_map[irq_line];
    uint32_t lo  = ioapic_read_reg(IOAPIC_RTE_LO(gsi));
    ioapic_write_reg(IOAPIC_RTE_LO(gsi), lo & ~IOAPIC_RTE_MASK_BIT);
}
