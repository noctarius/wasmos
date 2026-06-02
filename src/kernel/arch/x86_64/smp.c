#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu_x86_64.h"
#include "serial.h"
#include "paging.h"
#include "memory.h"

#include <stdint.h>

/* Per-CPU data array.  g_cpus[0] is the BSP.  g_cpus[1..g_cpu_count-1] are
 * APs populated by MADT discovery (WASMOS_SMP builds only). */
cpu_local_t g_cpus[WASMOS_MAX_CPUS];

/* Number of CPUs found in the MADT.  Always at least 1 (BSP). */
uint32_t g_cpu_count = 1;

#if WASMOS_SMP

#include "arch/x86_64/lapic.h"

/* Physical address where the AP trampoline code is placed. */
#define TRAMP_PHYS  0x1000ULL

/* Physical addresses of the AP data slots (below the trampoline). */
#define AP_SLOT_CR3          0x500ULL
#define AP_SLOT_ENTRY        0x508ULL
#define AP_SLOT_RSP          0x510ULL
#define AP_SLOT_GDTR         0x518ULL   /* 6 bytes: uint16_t limit + uint32_t base */
#define AP_SLOT_CPU_ID       0x51EULL   /* uint32_t */

/* SIPI vector: startup address = vector << 12 = 0x01 << 12 = 0x1000. */
#define TRAMP_SIPI_VECTOR   0x01u

/* Static stacks for APs.  Kept in BSS so they are in the kernel's higher-half
 * virtual address space and are covered by the existing page table. */
uint8_t g_ap_ist_stacks[WASMOS_MAX_CPUS - 1][CPU_IST_STACK_SIZE]
    __attribute__((aligned(16)));
uint8_t g_ap_rsp0_stacks[WASMOS_MAX_CPUS - 1][CPU_IST_STACK_SIZE]
    __attribute__((aligned(16)));

/* Trampoline symbol bounds exported from smp_trampoline.S. */
extern uint8_t smp_trampoline_start[];
extern uint8_t smp_trampoline_gdt[];
extern uint8_t smp_trampoline_end[];

/*
 * Write an 8-byte value to a physical address that has been identity-mapped
 * by smp_trampoline_setup() (virtual == physical for those pages).
 */
static inline void
write_slot_u64(uint64_t phys_addr, uint64_t value)
{
    *(volatile uint64_t *)(uintptr_t)phys_addr = value;
}

static inline void
write_slot_u32(uint64_t phys_addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)phys_addr = value;
}

static inline void
write_slot_u16(uint64_t phys_addr, uint16_t value)
{
    *(volatile uint16_t *)(uintptr_t)phys_addr = value;
}

/*
 * Set up the AP trampoline for the given AP slot:
 *   1. Identity-map the data-slot page (0x0000) and the trampoline page (0x1000).
 *   2. Copy the trampoline binary to physical 0x1000.
 *   3. Fill in data slots at 0x500 with CR3, entry point, stack, GDTR, cpu_id.
 */
static void
smp_trampoline_setup(cpu_local_t *ap, uint64_t ap_rsp)
{
    uint64_t data_flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE;
    uint64_t code_flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC;

    /* Identity-map physical page 0x0000 (covers data slots at 0x500). */
    paging_map_4k(0x0000ULL, 0x0000ULL, data_flags);
    /* Identity-map physical page 0x1000 (the trampoline code). */
    paging_map_4k(TRAMP_PHYS, TRAMP_PHYS, code_flags);

    /* Copy the trampoline binary to physical 0x1000. */
    uint32_t tramp_size = (uint32_t)(smp_trampoline_end - smp_trampoline_start);
    __builtin_memcpy((void *)(uintptr_t)TRAMP_PHYS, smp_trampoline_start, tramp_size);

    /* CR3: kernel page table root (physical, fits in 32 bits). */
    write_slot_u64(AP_SLOT_CR3, paging_get_root_table());

    /* Entry function: smp_ap_c_entry (kernel virtual address). */
    write_slot_u64(AP_SLOT_ENTRY, (uint64_t)(uintptr_t)smp_ap_c_entry);

    /* Initial RSP for the AP's boot stack. */
    write_slot_u64(AP_SLOT_RSP, ap_rsp);

    /* GDTR pointing at the temporary GDT embedded in the trampoline.
     * smp_trampoline_gdt offset from start → physical address after copy. */
    uint32_t gdt_phys = (uint32_t)(TRAMP_PHYS +
        (uintptr_t)(smp_trampoline_gdt - smp_trampoline_start));
    uint16_t gdt_limit = 5u * 8u - 1u;   /* 5 entries */
    write_slot_u16(AP_SLOT_GDTR,        gdt_limit);
    write_slot_u32(AP_SLOT_GDTR + 2ULL, gdt_phys);

    /* Logical CPU ID. */
    write_slot_u32(AP_SLOT_CPU_ID, ap->cpu_id);
}

/* C entry point called by the AP trampoline after entering 64-bit long mode. */
void
smp_ap_c_entry(uint32_t cpu_id)
{
    /* Perform per-CPU GDT/TSS/IDT/GS-base setup (runs on the AP). */
    x86_ap_cpu_init(cpu_id);

    /* Enable this AP's LAPIC and start its periodic timer at 250 Hz. */
    lapic_ap_enable(250u);

    /* Signal the BSP that this AP has completed initialisation. */
    __sync_synchronize();
    g_cpus[cpu_id].started = 1;

    /* Enable interrupts and idle. */
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/*
 * smp_init() — called by the BSP after LAPIC and I/O APIC are live.
 * Records the BSP's hardware APIC ID into g_cpus[0].apic_id and logs
 * the number of CPUs discovered by MADT scanning in ioapic_init().
 */
void
smp_init(void)
{
    g_cpus[0].apic_id = lapic_read_id();
    serial_printf("[smp] bsp apic_id=%u cpu_count=%u\n",
                  g_cpus[0].apic_id, g_cpu_count);
}

/*
 * smp_cpus_up() — bring up all APs discovered in g_cpus[1..g_cpu_count-1].
 *
 * For each AP:
 *   1. Allocate static interrupt stacks and populate the AP's cpu_local slot.
 *   2. Write the data slots and copy the trampoline to physical 0x1000.
 *   3. Send INIT IPI → SIPI → SIPI (Intel SDM INIT-SIPI-SIPI protocol).
 *   4. Busy-wait for ap->started to be set (with a timeout).
 *
 * APs are brought up one at a time so the shared data slots at 0x500 are
 * not clobbered before the previous AP has read them.
 */
void
smp_cpus_up(void)
{
    if (g_cpu_count <= 1u) {
        serial_write("[smp] no APs to bring up\n");
        return;
    }

    serial_printf("[smp] bringing up %u APs\n", g_cpu_count - 1u);

    for (uint32_t i = 1u; i < g_cpu_count; i++) {
        cpu_local_t *ap = &g_cpus[i];

        /* Allocate static stacks (in higher-half kernel BSS). */
        uint64_t ist1_top = (uint64_t)(uintptr_t)
            (g_ap_ist_stacks[i - 1u] + CPU_IST_STACK_SIZE);
        uint64_t rsp0_top = (uint64_t)(uintptr_t)
            (g_ap_rsp0_stacks[i - 1u] + CPU_IST_STACK_SIZE);

        /* Prepare the per-CPU GDT and TSS before the AP starts. */
        ap->cpu_id  = i;
        ap->started = 0;
        x86_cpu_prepare_ap(ap, ist1_top, rsp0_top);

        /* Write trampoline data slots and copy code to 0x1000. */
        smp_trampoline_setup(ap, rsp0_top);

        serial_printf("[smp] starting ap %u (apic_id=%u)\n",
                      i, ap->apic_id);

        /* INIT IPI → delay → SIPI → delay → SIPI (Intel SDM §10.4.4.1). */
        lapic_send_init_ipi(ap->apic_id);
        lapic_send_sipi(ap->apic_id, TRAMP_SIPI_VECTOR);
        lapic_send_sipi(ap->apic_id, TRAMP_SIPI_VECTOR);

        /* Wait for the AP to signal that it is fully initialised. */
        uint32_t timeout = 50000000u;
        while (!ap->started && timeout-- > 0u) {
            __asm__ volatile("pause");
        }

        if (ap->started) {
            serial_printf("[smp] ap %u online\n", i);
        } else {
            serial_printf("[smp] ap %u TIMEOUT — not responding\n", i);
        }
    }
}

#endif /* WASMOS_SMP */
