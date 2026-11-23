/* gic_aarch64.c - GICv3 on QEMU virt: distributor, the boot CPU's
 * redistributor, and the system-register CPU interface (ARM IHI 0069).
 *
 * Every interrupt is Group 1 with affinity routing; the machine has one
 * security state (QEMU virt without secure=on reports GICD_CTLR.DS = 1), so
 * the non-secure view is the whole controller.
 *
 * TODO: discover the distributor and redistributor bases from the ACPI MADT
 * (GICD/GICR entries) instead of QEMU virt's fixed addresses, and bring up
 * the redistributor of each secondary CPU (arm64 port, Phases 3-4). */
#include "gic_aarch64.h"

#include "arch/arch_cpu.h"
#include "arch/arch_mmio.h"
#include "arch/vm_layout.h"

#define GICR_PHYS 0x080A0000ULL
#define GICD_BASE (KERNEL_HIGHER_HALF_BASE | 0x08000000ULL)
#define GICR_BASE (KERNEL_HIGHER_HALF_BASE | GICR_PHYS)
#define GICR_SGI_BASE (GICR_BASE + 0x10000ULL)

#define GICD_CTLR 0x0000u
#define GICD_CTLR_RWP (1u << 31)
#define GICD_CTLR_ARE (1u << 4)
#define GICD_CTLR_ENABLE_G1 (1u << 1)
#define GICD_TYPER 0x0004u
#define GICD_IGROUPR 0x0080u
#define GICD_ISENABLER 0x0100u
#define GICD_ICENABLER 0x0180u
#define GICD_IPRIORITYR 0x0400u
#define GICD_ICFGR 0x0C00u
#define GICD_IROUTER 0x6000u

#define GICR_CTLR 0x0000u
#define GICR_CTLR_ENABLE_LPIS (1u << 0)
#define GICR_TYPER 0x0008u
#define GICR_TYPER_PLPIS (1ULL << 0)
#define GICR_PROPBASER 0x0070u
#define GICR_PENDBASER 0x0078u
/* Normal inner write-back read/write-allocate (0b111 at bits 9:7), inner
 * shareable (0b01 at bits 11:10): the attributes the GIC reads the LPI tables
 * with, matching the kernel's own mapping of them. */
#define GICR_BASER_CACHE_WB (7ULL << 7)
#define GICR_BASER_SHARE_INNER (1ULL << 10)
#define GICR_PENDBASER_PTZ (1ULL << 62)
#define GICR_WAKER 0x0014u
#define GICR_WAKER_PROCESSOR_SLEEP (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1u << 2)
#define GICR_IGROUPR0 0x0080u
#define GICR_ISENABLER0 0x0100u
#define GICR_ICENABLER0 0x0180u
#define GICR_IPRIORITYR 0x0400u

/* Lower values are higher priority; PMR 0xF0 lets every priority below it
 * through. */
#define GIC_DEFAULT_PRIORITY 0x80u
#define GIC_PMR_ALL 0xF0u

/* Interrupt lines the distributor implements (GICD_TYPER.ITLinesNumber). */
static uint32_t g_gicd_lines;

static void gicd_wait_rwp(void) {
    while (arch_mmio_read32((uintptr_t)(GICD_BASE + GICD_CTLR)) & GICD_CTLR_RWP) {
        arch_cpu_relax();
    }
}

void gic_init(void) {
    /* Distributor: off while reprogrammed, every SPI disabled, then on with
     * affinity routing and Group 1. */
    arch_mmio_write32((uintptr_t)(GICD_BASE + GICD_CTLR), 0);
    gicd_wait_rwp();
    uint32_t lines = ((arch_mmio_read32((uintptr_t)(GICD_BASE + GICD_TYPER)) & 0x1Fu) + 1u) * 32u;
    g_gicd_lines = lines;
    for (uint32_t i = 32; i < lines; i += 32) {
        arch_mmio_write32((uintptr_t)(GICD_BASE + GICD_ICENABLER + (uint64_t)(i / 32u) * 4u),
                          0xFFFFFFFFu);
    }
    gicd_wait_rwp();
    arch_mmio_write32((uintptr_t)(GICD_BASE + GICD_CTLR), GICD_CTLR_ARE | GICD_CTLR_ENABLE_G1);
    gicd_wait_rwp();

    /* Redistributor: wake it, then SGIs and PPIs Group 1, disabled. */
    uint32_t waker = arch_mmio_read32((uintptr_t)(GICR_BASE + GICR_WAKER));
    arch_mmio_write32((uintptr_t)(GICR_BASE + GICR_WAKER), waker & ~GICR_WAKER_PROCESSOR_SLEEP);
    while (arch_mmio_read32((uintptr_t)(GICR_BASE + GICR_WAKER)) & GICR_WAKER_CHILDREN_ASLEEP) {
        arch_cpu_relax();
    }
    arch_mmio_write32((uintptr_t)(GICR_SGI_BASE + GICR_ICENABLER0), 0xFFFFFFFFu);
    arch_mmio_write32((uintptr_t)(GICR_SGI_BASE + GICR_IGROUPR0), 0xFFFFFFFFu);

    /* CPU interface through system registers (ICC_SRE_EL1.SRE). */
    uint64_t sre;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));
    __asm__ volatile("msr icc_sre_el1, %0\n"
                     "isb" ::"r"(sre | 1u)
                     : "memory");
    __asm__ volatile("msr icc_pmr_el1, %0\n"
                     "msr icc_bpr1_el1, xzr\n"
                     "msr icc_igrpen1_el1, %1\n"
                     "isb" ::"r"((uint64_t)GIC_PMR_ALL),
                     "r"((uint64_t)1)
                     : "memory");
}

void gic_enable_ppi(uint32_t intid) {
    arch_mmio_write8((uintptr_t)(GICR_SGI_BASE + GICR_IPRIORITYR + intid), GIC_DEFAULT_PRIORITY);
    arch_mmio_write32((uintptr_t)(GICR_SGI_BASE + GICR_ISENABLER0), 1u << intid);
}

int gic_spi_valid(uint32_t intid) {
    return intid >= GIC_SPI_FIRST && intid < GIC_SPI_END && intid < g_gicd_lines;
}

void gic_spi_configure(uint32_t intid, int level) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    /* GICD_IROUTER<n>: Aff3 at 39:32, Aff2..Aff0 at 23:0, Interrupt_Routing_Mode
     * 0 (this one PE). */
    uint64_t route = ((mpidr >> 32) & 0xFFu) << 32 | (mpidr & 0xFFFFFFu);
    uintptr_t router = (uintptr_t)(GICD_BASE + GICD_IROUTER + (uint64_t)intid * 8u);
    arch_mmio_write32(router, (uint32_t)route);
    arch_mmio_write32(router + 4u, (uint32_t)(route >> 32));
    arch_mmio_write8((uintptr_t)(GICD_BASE + GICD_IPRIORITYR + intid), GIC_DEFAULT_PRIORITY);
    uintptr_t group = (uintptr_t)(GICD_BASE + GICD_IGROUPR + (intid / 32u) * 4u);
    arch_mmio_write32(group, arch_mmio_read32(group) | (1u << (intid % 32u)));
    /* GICD_ICFGR: two bits per INTID, the upper one set for edge-triggered. */
    uintptr_t cfg = (uintptr_t)(GICD_BASE + GICD_ICFGR + (intid / 16u) * 4u);
    uint32_t edge = 2u << ((intid % 16u) * 2u);
    uint32_t value = arch_mmio_read32(cfg);
    arch_mmio_write32(cfg, level ? (value & ~edge) : (value | edge));
}

void gic_spi_enable(uint32_t intid) {
    arch_mmio_write32((uintptr_t)(GICD_BASE + GICD_ISENABLER + (intid / 32u) * 4u),
                      1u << (intid % 32u));
}

void gic_spi_disable(uint32_t intid) {
    arch_mmio_write32((uintptr_t)(GICD_BASE + GICD_ICENABLER + (intid / 32u) * 4u),
                      1u << (intid % 32u));
    gicd_wait_rwp();
}

int gic_spi_enabled(uint32_t intid) {
    uint32_t value = arch_mmio_read32((uintptr_t)(GICD_BASE + GICD_ISENABLER + (intid / 32u) * 4u));
    return (value >> (intid % 32u)) & 1u;
}

static uint64_t gicr_read64(uint32_t reg) {
    uint64_t lo = arch_mmio_read32((uintptr_t)(GICR_BASE + reg));
    uint64_t hi = arch_mmio_read32((uintptr_t)(GICR_BASE + reg + 4u));
    return lo | (hi << 32);
}

static void gicr_write64(uint32_t reg, uint64_t value) {
    arch_mmio_write32((uintptr_t)(GICR_BASE + reg), (uint32_t)value);
    arch_mmio_write32((uintptr_t)(GICR_BASE + reg + 4u), (uint32_t)(value >> 32));
}

uint64_t gic_redistributor_phys(void) {
    return GICR_PHYS;
}

uint32_t gic_redistributor_processor_number(void) {
    return (uint32_t)((gicr_read64(GICR_TYPER) >> 8) & 0xFFFFu);
}

int gic_lpi_enable(uint64_t prop_phys, uint32_t id_bits, uint64_t pend_phys) {
    if (!(gicr_read64(GICR_TYPER) & GICR_TYPER_PLPIS)) {
        return -1;
    }
    gicr_write64(GICR_PROPBASER,
                 prop_phys | GICR_BASER_CACHE_WB | GICR_BASER_SHARE_INNER | (id_bits - 1u));
    gicr_write64(GICR_PENDBASER,
                 pend_phys | GICR_BASER_CACHE_WB | GICR_BASER_SHARE_INNER | GICR_PENDBASER_PTZ);
    uint32_t ctlr = arch_mmio_read32((uintptr_t)(GICR_BASE + GICR_CTLR));
    arch_mmio_write32((uintptr_t)(GICR_BASE + GICR_CTLR), ctlr | GICR_CTLR_ENABLE_LPIS);
    return 0;
}

uint32_t gic_ack(void) {
    uint64_t intid;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(intid)::"memory");
    __asm__ volatile("dsb sy" ::: "memory");
    return (uint32_t)(intid & 0xFFFFFFu);
}

void gic_eoi(uint32_t intid) {
    __asm__ volatile("msr icc_eoir1_el1, %0\n"
                     "isb" ::"r"((uint64_t)intid)
                     : "memory");
}
