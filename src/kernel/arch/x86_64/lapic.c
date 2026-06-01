#include "arch/x86_64/lapic.h"
#include "arch/x86_64/msr.h"
#include "irq.h"
#include "paging.h"
#include "serial.h"

#include <stdint.h>

/*
 * Local APIC (xAPIC mode) driver.  Reads the LAPIC physical base from the
 * IA32_APIC_BASE MSR, maps one 4 KB MMIO page into kernel virtual space, then
 * enables the LAPIC, disables the 8259 PIC, and programmes the LAPIC periodic
 * timer as the scheduler tick source at 250 Hz.
 *
 * The existing isr_irq_0 stub fires at vector IRQ_VECTOR_BASE (32) — the
 * LAPIC timer is aimed at the same vector so the rest of the scheduler path
 * (x86_timer_irq_handler → process_preempt_from_irq) requires no changes.
 */

/* IA32_APIC_BASE MSR (0x1B): bits [51:12] hold the physical base, bit 11 is
 * the global APIC enable flag, bit 8 marks the bootstrap processor. */
#define IA32_APIC_BASE_MSR     0x1Bu
#define IA32_APIC_BASE_EN      (1ULL << 11)
#define IA32_APIC_BASE_MASK    0x000FFFFFFFFFF000ULL

/* LAPIC MMIO register offsets (each register is 32-bit wide, 16-byte aligned). */
#define LAPIC_REG_ID           0x020u
#define LAPIC_REG_EOI          0x0B0u
#define LAPIC_REG_SVR          0x0F0u   /* Spurious Interrupt Vector Register */
#define LAPIC_REG_LVT_TIMER    0x320u
#define LAPIC_REG_TIMER_ICR    0x380u   /* Initial Count */
#define LAPIC_REG_TIMER_CCR    0x390u   /* Current Count */
#define LAPIC_REG_TIMER_DCR    0x3E0u   /* Divide Configuration */

/* SVR bit 8: software-enable; low 8 bits: spurious vector. */
#define LAPIC_SVR_ENABLE       (1u << 8)
#define LAPIC_SPURIOUS_VECTOR  0xFFu

/* LVT timer bits [18:17] — 00=one-shot, 01=periodic, 10=TSC-deadline. */
#define LAPIC_TIMER_PERIODIC   (1u << 17)

/* Divide-by-16: DCR value 0x3. */
#define LAPIC_TIMER_DIVIDE_16  0x3u

/* PT_FLAG_PCD (bit 4): cache-disable, required for MMIO mappings. */
#define PT_FLAG_PRESENT        (1ULL << 0)
#define PT_FLAG_WRITE          (1ULL << 1)
#define PT_FLAG_PCD            (1ULL << 4)
#define PT_FLAG_NX             (1ULL << 63)

/*
 * Reserved kernel virtual address for the LAPIC MMIO page.
 * Sits in the 2 MB gap between KERNEL_HIGHER_HALF_BASE (0xFFFFFFFF80000000)
 * and the first kernel image page (0xFFFFFFFF80200000) — guaranteed free in
 * the initial kernel page tables since that range is never covered by the
 * large-page kernel mapping.
 */
#define LAPIC_VIRT_BASE        0xFFFFFFFF80001000ULL

/* PIT channel 2 registers — used as a reference clock for calibration only. */
#define PIT_CMD_PORT           0x43u
#define PIT_CH2_PORT           0x42u
#define PIT_GATE_PORT          0x61u
#define PIT_BASE_HZ            1193182u

static uintptr_t g_lapic_base;  /* kernel virtual address of LAPIC MMIO */

/* ------------------------------------------------------------------ helpers */

static inline void
outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t
lapic_read(uint32_t reg)
{
    volatile uint32_t *addr = (volatile uint32_t *)(g_lapic_base + reg);
    return *addr;
}

static inline void
lapic_write(uint32_t reg, uint32_t val)
{
    volatile uint32_t *addr = (volatile uint32_t *)(g_lapic_base + reg);
    *addr = val;
}

/* ---------------------------------------------------------------- map/enable */

static void
lapic_map(void)
{
    uint64_t apic_base_msr = x86_read_msr(IA32_APIC_BASE_MSR);
    uint64_t lapic_phys    = apic_base_msr & IA32_APIC_BASE_MASK;
    uint64_t flags         = PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_PCD | PT_FLAG_NX;

    int rc = paging_map_4k(LAPIC_VIRT_BASE, lapic_phys, flags);
    if (rc != 0) {
        serial_write("[lapic] mmio map failed\n");
        return;
    }
    g_lapic_base = (uintptr_t)LAPIC_VIRT_BASE;
}

static void
lapic_enable(void)
{
    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr &= ~0xFFu;
    svr |= LAPIC_SPURIOUS_VECTOR;
    svr |= LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_REG_SVR, svr);

    /* Ensure the global enable bit in the MSR is set (may already be set). */
    uint64_t apic_base_msr = x86_read_msr(IA32_APIC_BASE_MSR);
    if (!(apic_base_msr & IA32_APIC_BASE_EN)) {
        x86_write_msr(IA32_APIC_BASE_MSR, apic_base_msr | IA32_APIC_BASE_EN);
    }
}

/* --------------------------------------------------------- 8259 PIC disable */

static void
pic_disable(void)
{
    /* Mask every line on both PICs so no legacy interrupt reaches the CPU. */
    outb(0x21u, 0xFFu);
    outb(0xA1u, 0xFFu);
}

/* --------------------------------- PIT channel 2 calibration (one-shot 10 ms) */

static uint32_t
lapic_calibrate_ticks_per_ms(void)
{
    /*
     * Use PIT channel 2 in mode 0 (interrupt-on-terminal-count / one-shot)
     * as a ~10 ms reference.  Channel 2 output is readable from bit 5 of
     * port 0x61 without requiring an IRQ, so it does not disturb the PIC or
     * the existing timer ISR path.
     *
     * 10 ms at 1193182 Hz ≈ 11932 (0x2E9C) PIT ticks.
     */
    uint8_t gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (gate & ~0x02u) | 0x01u);  /* gate on, speaker off */
    outb(PIT_CMD_PORT, 0xB0u);  /* ch2, mode 0, binary, LSB+MSB */
    outb(PIT_CH2_PORT, 0x9Cu);  /* low byte:  11932 & 0xFF */
    outb(PIT_CH2_PORT, 0x2Eu);  /* high byte: 11932 >> 8  */

    /* Start LAPIC counter at maximum so we can measure elapsed ticks. */
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIVIDE_16);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFFu);

    /* Busy-wait: bit 5 of port 0x61 goes high when channel 2 output fires. */
    while ((inb(PIT_GATE_PORT) & 0x20u) == 0u)
        ;

    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    uint32_t elapsed   = 0xFFFFFFFFu - remaining;

    /* Disable channel 2 gate. */
    outb(PIT_GATE_PORT, inb(PIT_GATE_PORT) & ~0x01u);

    return elapsed / 10u;  /* ticks per millisecond */
}

/* ---------------------------------------------------------- timer configure */

static void
lapic_timer_set_hz(uint32_t hz)
{
    if (hz == 0u) {
        hz = 250u;
    }

    uint32_t ticks_per_ms  = lapic_calibrate_ticks_per_ms();
    uint32_t initial_count = (ticks_per_ms * 1000u) / hz;

    /* Mask the timer entry while reconfiguring. */
    lapic_write(LAPIC_REG_LVT_TIMER, (1u << 16));

    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIVIDE_16);
    /* Periodic mode (bit 17), vector = IRQ_VECTOR_BASE (32 = 0x20).
     * This reuses the existing isr_irq_0 → x86_timer_irq_handler path. */
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_PERIODIC | IRQ_VECTOR_BASE);
    lapic_write(LAPIC_REG_TIMER_ICR, initial_count);
}

/* --------------------------------------------------------------------- API */

void
lapic_init(uint32_t hz)
{
    lapic_map();
    lapic_enable();
    pic_disable();
    lapic_timer_set_hz(hz);
    serial_write("[lapic] init ok\n");
}

void
lapic_eoi(void)
{
    lapic_write(LAPIC_REG_EOI, 0u);
}
