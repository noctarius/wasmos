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

#define GICD_BASE (KERNEL_HIGHER_HALF_BASE | 0x08000000ULL)
#define GICR_BASE (KERNEL_HIGHER_HALF_BASE | 0x080A0000ULL)
#define GICR_SGI_BASE (GICR_BASE + 0x10000ULL)

#define GICD_CTLR 0x0000u
#define GICD_CTLR_RWP (1u << 31)
#define GICD_CTLR_ARE (1u << 4)
#define GICD_CTLR_ENABLE_G1 (1u << 1)
#define GICD_TYPER 0x0004u
#define GICD_ICENABLER 0x0180u

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
