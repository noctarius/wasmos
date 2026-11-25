/* gic_aarch64.c - GICv3 on QEMU virt: distributor, the boot CPU's
 * redistributor, and the system-register CPU interface (ARM IHI 0069).
 *
 * Every interrupt is Group 1 with affinity routing, except the stop SGI. The
 * machine has one security state (QEMU virt without secure=on reports
 * GICD_CTLR.DS = 1), so the non-secure view is the whole controller -- and
 * Group 0 is then signalled to EL1 as an FIQ. The stop SGI is Group 0 on such a
 * controller: the kernel masks only PSTATE.I, so the stop reaches a CPU that
 * spins with interrupts masked. With two security states Group 0 belongs to the
 * secure world and the stop stays an ordinary Group 1 SGI.
 *
 * Each CPU has its own redistributor; gic_cpu_init finds it among the
 * contiguous frames at QEMU virt's GICR base and records it in cpu_local, and
 * every redistributor access below goes through the calling CPU's frame.
 * Device interrupts (SPIs) and the ITS collection target the boot CPU.
 *
 * TODO: discover the distributor and redistributor bases from the ACPI MADT
 * (GICD/GICR entries) instead of QEMU virt's fixed addresses. */
#include "gic_aarch64.h"

#include "arch/arch_cpu.h"
#include "arch/arch_mmio.h"
#include "arch/vm_layout.h"
#include "cpu_local.h"

#define GICR_PHYS 0x080A0000ULL
#define GICD_BASE (KERNEL_HIGHER_HALF_BASE | 0x08000000ULL)
#define GICR_REGION_BASE (KERNEL_HIGHER_HALF_BASE | GICR_PHYS)
/* The calling CPU's redistributor frame (RD_base) and its SGI/PPI frame. */
#define GICR_BASE (cpu_local()->arch.gicr_base)
#define GICR_SGI_BASE (GICR_BASE + 0x10000ULL)
/* Redistributor frames per CPU: RD_base + SGI_base, plus VLPI_base and a
 * reserved frame when GICR_TYPER.VLPIS is set (GICv3 9.10). */
#define GICR_FRAME_STRIDE 0x20000ULL
#define GICR_FRAME_STRIDE_VLPI 0x40000ULL
/* Bound on the walk; QEMU virt's region holds 123 redistributors. */
#define GICR_MAX_FRAMES 128u

#define GICD_CTLR 0x0000u
#define GICD_CTLR_RWP (1u << 31)
#define GICD_CTLR_ARE (1u << 4)
#define GICD_CTLR_ENABLE_G1 (1u << 1)
/* GICD_CTLR with DS = 1 (single security state view). */
#define GICD_CTLR_ENABLE_G0 (1u << 0)
#define GICD_CTLR_DS (1u << 6)
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
#define GICR_TYPER_VLPIS (1ULL << 1)
#define GICR_TYPER_LAST (1ULL << 4)
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
/* The stop SGI's priority, strictly higher (numerically lower) than every
 * other interrupt's: a CPU inside a handler runs at that interrupt's priority
 * until its end of interrupt, and the GIC signals only a higher-priority one,
 * so at GIC_DEFAULT_PRIORITY the stop would not reach a CPU stuck there. */
#define GIC_STOP_PRIORITY 0x40u
#define GIC_PMR_ALL 0xF0u

/* Interrupt lines the distributor implements (GICD_TYPER.ITLinesNumber). */
static uint32_t g_gicd_lines;
/* Set by gic_init when the stop SGI is Group 0 (an FIQ); see the file comment.
 * Written once on the boot CPU before any secondary starts. */
static uint8_t g_gic_stop_fiq;

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
    if (arch_mmio_read32((uintptr_t)(GICD_BASE + GICD_CTLR)) & GICD_CTLR_DS) {
        arch_mmio_write32((uintptr_t)(GICD_BASE + GICD_CTLR),
                          GICD_CTLR_ARE | GICD_CTLR_ENABLE_G1 | GICD_CTLR_ENABLE_G0);
        gicd_wait_rwp();
        g_gic_stop_fiq = 1;
    }
    (void)gic_cpu_init();
}

static uint64_t gicr_frame_read64(uintptr_t frame, uint32_t reg) {
    uint64_t lo = arch_mmio_read32(frame + reg);
    uint64_t hi = arch_mmio_read32(frame + reg + 4u);
    return lo | (hi << 32);
}

/* The redistributor whose GICR_TYPER.Affinity_Value (bits 63:32: Aff3, Aff2,
 * Aff1, Aff0) names `mpidr`, or 0. */
static uintptr_t gicr_find(uint64_t mpidr) {
    uint32_t affinity = (uint32_t)(((mpidr >> 32) & 0xFFu) << 24 | (mpidr & 0xFFFFFFu));
    uintptr_t frame = (uintptr_t)GICR_REGION_BASE;
    for (uint32_t i = 0; i < GICR_MAX_FRAMES; ++i) {
        uint64_t typer = gicr_frame_read64(frame, GICR_TYPER);
        if ((uint32_t)(typer >> 32) == affinity) {
            return frame;
        }
        if (typer & GICR_TYPER_LAST) {
            break;
        }
        frame += (typer & GICR_TYPER_VLPIS) ? GICR_FRAME_STRIDE_VLPI : GICR_FRAME_STRIDE;
    }
    return 0;
}

int gic_cpu_init(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uintptr_t frame = gicr_find(mpidr);
    if (!frame) {
        return -1;
    }
    cpu_local()->arch.gicr_base = frame;

    /* Redistributor: wake it, then SGIs and PPIs Group 1, disabled. */
    uint32_t waker = arch_mmio_read32((uintptr_t)(GICR_BASE + GICR_WAKER));
    arch_mmio_write32((uintptr_t)(GICR_BASE + GICR_WAKER), waker & ~GICR_WAKER_PROCESSOR_SLEEP);
    while (arch_mmio_read32((uintptr_t)(GICR_BASE + GICR_WAKER)) & GICR_WAKER_CHILDREN_ASLEEP) {
        arch_cpu_relax();
    }
    arch_mmio_write32((uintptr_t)(GICR_SGI_BASE + GICR_ICENABLER0), 0xFFFFFFFFu);
    uint32_t group1 = g_gic_stop_fiq ? ~(1u << GIC_SGI_STOP) : 0xFFFFFFFFu;
    arch_mmio_write32((uintptr_t)(GICR_SGI_BASE + GICR_IGROUPR0), group1);

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
    /* The stop request every CPU must be able to take. As Group 0 it arrives
     * as an FIQ, which this CPU takes from here on: PSTATE.F stays clear
     * outside the exception entry and exit (aarch64_exception). */
    arch_mmio_write8((uintptr_t)(GICR_SGI_BASE + GICR_IPRIORITYR + GIC_SGI_STOP),
                     GIC_STOP_PRIORITY);
    arch_mmio_write32((uintptr_t)(GICR_SGI_BASE + GICR_ISENABLER0), 1u << GIC_SGI_STOP);
    if (g_gic_stop_fiq) {
        __asm__ volatile("msr icc_bpr0_el1, xzr\n"
                         "msr icc_igrpen0_el1, %0\n"
                         "isb\n"
                         "msr daifclr, #1" ::"r"((uint64_t)1)
                         : "memory");
    }
    return 0;
}

void gic_enable_ppi(uint32_t intid) {
    arch_mmio_write8((uintptr_t)(GICR_SGI_BASE + GICR_IPRIORITYR + intid), GIC_DEFAULT_PRIORITY);
    arch_mmio_write32((uintptr_t)(GICR_SGI_BASE + GICR_ISENABLER0), 1u << intid);
}

void gic_send_stop_others(void) {
    if (!g_gic_stop_fiq) {
        gic_send_sgi_others(GIC_SGI_STOP);
        return;
    }
    /* ICC_SGI0R_EL1, laid out as ICC_SGI1R_EL1: a Group 0 SGI. */
    uint64_t value = ((uint64_t)GIC_SGI_STOP << 24) | (1ULL << 40);
    __asm__ volatile("dsb ishst\n"
                     "msr icc_sgi0r_el1, %0\n"
                     "isb" ::"r"(value)
                     : "memory");
}

uint32_t gic_fiq_ack(void) {
    uint64_t intid;
    __asm__ volatile("mrs %0, icc_iar0_el1" : "=r"(intid)::"memory");
    return (uint32_t)(intid & 0xFFFFFFu);
}

void gic_fiq_eoi(uint32_t intid) {
    __asm__ volatile("msr icc_eoir0_el1, %0\n"
                     "isb" ::"r"((uint64_t)intid)
                     : "memory");
}

void gic_send_sgi_others(uint32_t intid) {
    /* ICC_SGI1R_EL1: INTID at 27:24, IRM (bit 40) = 1 routes to every PE but
     * the sender, ignoring the target list. The barrier orders the caller's
     * prior stores before the SGI is generated. */
    uint64_t value = ((uint64_t)(intid & 0xFu) << 24) | (1ULL << 40);
    __asm__ volatile("dsb ishst\n"
                     "msr icc_sgi1r_el1, %0\n"
                     "isb" ::"r"(value)
                     : "memory");
}

int gic_spi_valid(uint32_t intid) {
    return intid >= GIC_SPI_FIRST && intid < GIC_SPI_END && intid < g_gicd_lines;
}

void gic_spi_configure(uint32_t intid, int level) {
    uint64_t mpidr = g_cpus[0].arch.mpidr;
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

/* The boot CPU's redistributor: the ITS collection target and the one LPIs are
 * enabled on, whichever CPU asks. */
#define GICR_BOOT_BASE (g_cpus[0].arch.gicr_base)

static uint64_t gicr_read64(uint32_t reg) {
    return gicr_frame_read64(GICR_BOOT_BASE, reg);
}

static void gicr_write64(uint32_t reg, uint64_t value) {
    arch_mmio_write32((uintptr_t)(GICR_BOOT_BASE + reg), (uint32_t)value);
    arch_mmio_write32((uintptr_t)(GICR_BOOT_BASE + reg + 4u), (uint32_t)(value >> 32));
}

uint64_t gic_redistributor_phys(void) {
    return (uint64_t)(GICR_BOOT_BASE - (uintptr_t)KERNEL_HIGHER_HALF_BASE);
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
    uint32_t ctlr = arch_mmio_read32((uintptr_t)(GICR_BOOT_BASE + GICR_CTLR));
    arch_mmio_write32((uintptr_t)(GICR_BOOT_BASE + GICR_CTLR), ctlr | GICR_CTLR_ENABLE_LPIS);
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
