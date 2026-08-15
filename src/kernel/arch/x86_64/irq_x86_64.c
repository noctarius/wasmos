#include "irq.h"
#include "arch/x86_64/irq_x86_64.h"
#include "ipc.h"
#include "serial.h"
#include "kpanic.h"
#include "timer.h"
#include "policy.h"
#include "sync/spinlock.h"
#include "paging.h"
#if WASMOS_IRQ_MODE >= 1
#include "arch/x86_64/lapic.h"
#endif
#if WASMOS_IRQ_MODE == 2
#include "arch/x86_64/ioapic.h"
#endif

/*
 * x86_64 interrupt-controller backend: 8259/IOAPIC programming, EOI discipline
 * and the interrupt-side entry points behind the arch-neutral irq.c shim. The
 * policy rule is strict: do the smallest safe amount of work in interrupt
 * context, then let the scheduler and regular kernel paths finish the job.
 *
 * WASMOS_IRQ_MODE selects the delivery path and is fixed at build time:
 *   0 — 8259 PIC only, PIT timer, no LAPIC (msi_alloc refuses).
 *   1 — LAPIC timer on IRQ 0, device IRQs 1-15 from the 8259 via LINT0 ExtINT.
 *   2 — IOAPIC; the 8259 is masked off entirely. Required for SMP.
 */

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define PIC_EOI 0x20

/* IPC message type sent to endpoint when an IRQ fires. */
#define IPC_IRQ_EVENT_TYPE 0xFF00u

/* Per-line sharer bookkeeping lives in irq_sharing.c (arch-neutral and
 * host-unit-tested); this file supplies the hardware effects and the table.
 *
 * The table is mutated from interrupt context on any CPU (dispatch, tick scan)
 * and from hostcalls on any other (register/ack/unregister), so every entry into
 * the module is serialised by this lock.  It is NOT optional under SMP: the state
 * includes counters (pending_acks, the dispatch budget) whose read-modify-write
 * races on real concurrent CPUs, and a lost decrement leaves the line masked
 * forever — i.e. the device dead.  spinlock_lock() disables interrupts and
 * preemption, so it is safe to take from an ISR. */
static irq_line_t g_irq_lines[IRQ_COUNT];
static ksync_spinlock_t g_irq_lines_lock;
/* PIC mask state used in modes 0 (direct PIC) and 1 (PIC via LINT0 ExtINT). */
#if WASMOS_IRQ_MODE <= 1
static uint8_t g_pic_mask1 = 0xFF;
static uint8_t g_pic_mask2 = 0xFF;
#endif

static inline uintptr_t irq_alias_ptr(uintptr_t p) {
    if (serial_high_alias_enabled() && (uint64_t)p < KERNEL_HIGHER_HALF_BASE) {
        p = (uintptr_t)((uint64_t)p + KERNEL_HIGHER_HALF_BASE);
    }
    return p;
}

static inline irq_line_t* irq_lines_ptr(void) {
    return (irq_line_t*)(void*)irq_alias_ptr((uintptr_t)&g_irq_lines[0]);
}

#if WASMOS_IRQ_MODE <= 1
static inline uint8_t* pic_mask1_slot(void) {
    return (uint8_t*)(void*)irq_alias_ptr((uintptr_t)&g_pic_mask1);
}

static inline uint8_t* pic_mask2_slot(void) {
    return (uint8_t*)(void*)irq_alias_ptr((uintptr_t)&g_pic_mask2);
}
#endif

/* Report and halt after the IRQ 0 stub found its IRET frame altered in a way the
 * preemption rewrite does not explain. saved points at the three-quadword copy
 * (rip, cs, rflags) the stub took on entry, current at the live frame; both are
 * borrowed. Called from assembly only, and does not return: it always ends in
 * kpanic(), including when either pointer is NULL. */
void x86_irq_iret_corrupt(const uint64_t* saved, const uint64_t* current) {
    serial_write("[irq] iret frame corrupt\n");
    if (!saved || !current) {
        kpanic("irq iret frame corrupt (invalid ptr)", 0ULL, 0ULL);
    }
    serial_printf("[irq] saved rip=%016llx\n"
                  "[irq] saved cs=%016llx\n"
                  "[irq] saved rflags=%016llx\n"
                  "[irq] current rip=%016llx\n"
                  "[irq] current cs=%016llx\n"
                  "[irq] current rflags=%016llx\n",
                  (unsigned long long)saved[0],
                  (unsigned long long)saved[1],
                  (unsigned long long)saved[2],
                  (unsigned long long)current[0],
                  (unsigned long long)current[1],
                  (unsigned long long)current[2]);
    /* Corrupt return frame — unrecoverable. a=saved rip, b=current rip. */
    kpanic("irq_iret_frame_corrupt", saved[0], current[0]);
}

/* Report and halt after the IRQ 0 stub found the IST1 guard word overwritten,
 * i.e. the interrupt stack grew down into its own base. Called from assembly on
 * the very stack that overflowed, so it takes no arguments and does no work
 * beyond kpanic(); it does not return. */
void x86_irq_ist_corrupt(void) {
    kpanic("irq_ist_stack_canary_corrupt", 0ULL, 0ULL);
}

/* PIC I/O helpers are shared by mode 0 (direct PIC) and mode 1 (PIC via
 * LINT0 ExtINT).  Mode 2 (IOAPIC) disables the PIC entirely and never calls
 * these. */
#if WASMOS_IRQ_MODE <= 1
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void io_wait(void) {
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0x80));
}

static void pic_write_masks(void) {
    outb(PIC1_DATA, *pic_mask1_slot());
    outb(PIC2_DATA, *pic_mask2_slot());
}

static int pic_is_spurious(uint32_t irq_line) {
    if (irq_line == 7) {
        outb(PIC1_CMD, 0x0B);
        uint8_t isr = inb(PIC1_CMD);
        return (isr & (1u << 7)) == 0;
    }
    if (irq_line == 15) {
        outb(PIC2_CMD, 0x0B);
        uint8_t isr = inb(PIC2_CMD);
        return (isr & (1u << 7)) == 0;
    }
    return 0;
}
#endif /* WASMOS_IRQ_MODE <= 1 */

static void irq_send_eoi(uint32_t irq_line) {
#if WASMOS_IRQ_MODE == 0
    /* Pure PIC mode: all IRQs go through the 8259 — always send PIC EOI. */
    if (irq_line >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
#elif WASMOS_IRQ_MODE == 1
    /*
     * LAPIC + PIC-via-ExtINT mode: two distinct delivery paths.
     *
     * IRQ 0 (timer) fires through the LAPIC LVT_TIMER entry, which sets the
     * LAPIC ISR bit — clear it with a LAPIC EOI write.
     *
     * IRQ 1–15 (devices) arrive via LINT0 in ExtINT delivery mode: the LAPIC
     * acts as a transparent pass-through and does NOT set any ISR bit (Intel
     * SDM Vol 3A, Local APIC chapter, "Signaling Interrupt Servicing
     * Completion").  Only the 8259 PIC needs an EOI to clear its ISR.
     * Sending a LAPIC EOI here would erroneously clear whatever ISR bit the
     * LAPIC currently has set (e.g. a concurrently-pending timer tick).
     */
    if (irq_line == 0) {
        lapic_eoi();
    } else {
        if (irq_line >= 8) {
            outb(PIC2_CMD, PIC_EOI);
        }
        outb(PIC1_CMD, PIC_EOI);
    }
#else
    /* IOAPIC mode: LAPIC EOI covers both the LAPIC ISR and (for level-triggered
     * RTEs) broadcasts the EOIS back to the IOAPIC to clear Remote IRR. */
    (void)irq_line;
    lapic_eoi();
#endif
}

/* Early interrupt-controller setup, called from x86_cpu_init() on the BSP with
 * interrupts still masked. Initialises the sharing table and, in modes 0 and 1,
 * remaps the 8259 pair to vectors IRQ_VECTOR_BASE..+15 while preserving the
 * firmware's mask state, so no line becomes live here. The IOAPIC half of mode 2
 * is not done now: it needs the ACPI tables and happens later in
 * x86_irq_late_init(). */
void x86_irq_init(void) {
    ksync_spinlock_init(&g_irq_lines_lock);
    irq_sharing_init(irq_lines_ptr(), IRQ_COUNT);

/*
 * Remap the 8259 PIC to vectors 32–47 in both mode 0 (direct PIC) and
 * mode 1 (PIC via LINT0 ExtINT).  In mode 1 the PIC handles device IRQs
 * 1–15 while the LAPIC timer drives IRQ 0; the remap prevents legacy
 * vectors 8–15 from overlapping CPU exception vectors.  In mode 2 (IOAPIC)
 * the PIC is disabled later by lapic_init() so no remap is needed.
 */
#if WASMOS_IRQ_MODE <= 1
    uint8_t* mask1_slot = pic_mask1_slot();
    uint8_t* mask2_slot = pic_mask2_slot();

    /* Preserve the pre-existing mask state across the PIC remap so only the
     * lines explicitly unmasked later become active. */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC1_DATA, IRQ_VECTOR_BASE);
    io_wait();
    outb(PIC2_DATA, IRQ_VECTOR_BASE + 8);
    io_wait();
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    *mask1_slot = mask1;
    *mask2_slot = mask2;
    pic_write_masks();
    serial_write("[irq] pic remapped\n");
#endif
}

/* Mask/unmask one ISA IRQ line at whichever controller the build selected: the
 * 8259 mask registers in modes 0 and 1, the matching IOAPIC redirection entry in
 * mode 2. Return 0 on success and -1 for a line >= IRQ_COUNT; the -1 is an
 * internal-only value, since both entry points are reached from the irq_sharing
 * ops table rather than from a host call. Unmasking a slave line (>= 8) also
 * clears the master's cascade bit, without which no slave IRQ is delivered.
 * Locking: the IOAPIC path takes the IOAPIC register lock internally. The PIC
 * path takes none and read-modify-writes the cached mask bytes, so callers that
 * can run concurrently must serialise themselves; the irq_sharing ops path does,
 * by holding g_irq_lines_lock. */
int x86_irq_mask(uint32_t irq_line) {
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
#if WASMOS_IRQ_MODE <= 1
    uint8_t* mask1_slot = pic_mask1_slot();
    uint8_t* mask2_slot = pic_mask2_slot();
    if (irq_line < 8) {
        *mask1_slot |= (uint8_t)(1u << irq_line);
    } else {
        *mask2_slot |= (uint8_t)(1u << (irq_line - 8));
    }
    pic_write_masks();
#elif WASMOS_IRQ_MODE == 2
    ioapic_mask_irq(irq_line);
#endif
    return 0;
}

int x86_irq_unmask(uint32_t irq_line) {
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
#if WASMOS_IRQ_MODE <= 1
    uint8_t* mask1_slot = pic_mask1_slot();
    uint8_t* mask2_slot = pic_mask2_slot();
    if (irq_line < 8) {
        *mask1_slot &= (uint8_t)~(1u << irq_line);
    } else {
        *mask2_slot &= (uint8_t)~(1u << (irq_line - 8));
        /* The slave PIC feeds the master via the cascade line (IRQ 2).
         * UEFI runs in APIC mode and typically leaves the 8259 fully masked
         * (mask1=0xFF), so bit 2 of the master mask may never have been
         * cleared.  Without it, no slave IRQ (8-15) reaches the CPU. */
        *mask1_slot &= (uint8_t)~(1u << 2);
    }
    pic_write_masks();
#elif WASMOS_IRQ_MODE == 2
    ioapic_unmask_irq(irq_line);
#endif
    return 0;
}

/* Set trigger/polarity for a line. flags: bit0 = level-triggered,
 * bit1 = active-low. Only meaningful in IOAPIC mode; the 8259 PIC has no
 * per-line polarity control, so this is a no-op there.
 * FIXME: an out-of-range line returns a bare -1, yet this is reachable from a
 * guest through the irq_configure host call, so the caller cannot tell it apart
 * from any other failure. It must return the packed WASMOS_ERR_IRQ_BAD_LINE the
 * neighbouring entry points use. */
int x86_irq_configure(uint32_t irq_line, uint32_t flags) {
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
#if WASMOS_IRQ_MODE == 2
    ioapic_configure_irq(irq_line, (flags & 1u) != 0, (flags & 2u) != 0);
#else
    (void)flags;
#endif
    return 0;
}

/* Second-stage interrupt-controller setup, run once the ACPI tables from
 * boot_info are reachable. In IOAPIC mode this parses the MADT and programs the
 * redirection entries (all initially masked); in every other mode it is a no-op
 * and boot_info is unused. boot_info is borrowed and must carry a valid rsdp. */
void x86_irq_late_init(const boot_info_t* boot_info) {
#if WASMOS_IRQ_MODE == 2
    ioapic_init(boot_info);
#else
    (void)boot_info;
#endif
}

/* Hardware effects and delivery for irq_sharing.c. */
static void irq_ops_mask(uint32_t line) {
    (void)x86_irq_mask(line);
}

static void irq_ops_unmask(uint32_t line) {
    (void)x86_irq_unmask(line);
}

static int irq_ops_deliver(uint32_t endpoint, uint32_t line) {
    ipc_message_t irq_msg;
    irq_msg.type = IPC_IRQ_EVENT_TYPE;
    irq_msg.request_id = (int32_t)line;
    irq_msg.source = IPC_ENDPOINT_NONE;
    irq_msg.destination = endpoint;
    irq_msg.arg0 = (int32_t)line;
    irq_msg.arg1 = 0;
    irq_msg.arg2 = 0;
    irq_msg.arg3 = 0;
    return ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, &irq_msg) == IPC_OK ? 0 : -1;
}

static void irq_ops_log_throttle(uint32_t line) {
    /* serial_write_hex64 terminates the line, so the number goes last. */
    serial_write("[irq] dispatch budget exhausted (unclaimed assertion or runaway device),"
                 " throttling line=");
    serial_write_hex64(line);
}

static const irq_sharing_ops_t g_irq_ops = {
    irq_ops_mask,
    irq_ops_unmask,
    irq_ops_deliver,
    timer_ticks,
    irq_ops_log_throttle,
};

/* Route an IRQ line to an IPC endpoint on behalf of a guest context.
 *
 * Returns 0 on success, or a packed abi/errors.yaml code: WASMOS_ERR_IRQ_BAD_LINE
 * for a line >= IRQ_COUNT, WASMOS_ERR_IRQ_BAD_ENDPOINT for IPC_ENDPOINT_NONE or
 * an endpoint the calling context does not own, WASMOS_ERR_IRQ_NOT_AUTHORIZED
 * when policy denies the route, and whatever irq_sharing_register() returns
 * otherwise (WASMOS_ERR_IRQ_LINE_FULL when the line has no free sharer slot).
 * Lines are shareable, and re-registering a context that already holds the line
 * updates its endpoint rather than adding a second entry.
 * Takes g_irq_lines_lock, which masks interrupts for the duration. */
int x86_irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    if (irq_line >= IRQ_COUNT) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    if (endpoint == IPC_ENDPOINT_NONE) {
        return WASMOS_ERR_IRQ_BAD_ENDPOINT;
    }
    if (policy_authorize(context_id, POLICY_ACTION_IRQ_ROUTE, irq_line) != 0) {
        return WASMOS_ERR_IRQ_NOT_AUTHORIZED;
    }
    uint32_t owner_context_id = 0;
    if (ipc_endpoint_owner(endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id != context_id) {
        return WASMOS_ERR_IRQ_BAD_ENDPOINT;
    }
    ksync_spinlock_lock(&g_irq_lines_lock);
    int rc = irq_sharing_register(irq_lines_ptr(), irq_line, context_id, endpoint, &g_irq_ops);
    ksync_spinlock_unlock(&g_irq_lines_lock);
    return rc;
}

/* Report that a driver has serviced its device for one delivery of irq_line.
 * The line is unmasked once every sharer that was notified has acked, so a
 * driver that never acks leaves the line dead until the tick-driven deadline
 * forces completion. Returns 0 on success — including an ack nothing was waiting
 * for, since drivers may ack defensively — WASMOS_ERR_IRQ_BAD_LINE for a line
 * >= IRQ_COUNT, or WASMOS_ERR_IRQ_NOT_A_SHARER when the context does not hold
 * the line. Takes g_irq_lines_lock. */
int x86_irq_ack(uint32_t context_id, uint32_t irq_line) {
    if (irq_line >= IRQ_COUNT) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    ksync_spinlock_lock(&g_irq_lines_lock);
    int rc = irq_sharing_ack(irq_lines_ptr(), irq_line, context_id, &g_irq_ops);
    ksync_spinlock_unlock(&g_irq_lines_lock);
    return rc;
}

/* Drop a route. A normal context removes only its own sharer entry and any ack
 * it still owed; IPC_CONTEXT_KERNEL is privileged and removes every sharer of the
 * line. Returns 0 on success, WASMOS_ERR_IRQ_BAD_LINE for a line >= IRQ_COUNT, or
 * WASMOS_ERR_IRQ_NOT_A_SHARER when there was nothing to remove.
 * Takes g_irq_lines_lock. */
int x86_irq_unregister(uint32_t context_id, uint32_t irq_line) {
    if (irq_line >= IRQ_COUNT) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    ksync_spinlock_lock(&g_irq_lines_lock);
    int rc = (context_id == IPC_CONTEXT_KERNEL)
                 ? irq_sharing_unregister_all(irq_lines_ptr(), irq_line, &g_irq_ops)
                 : irq_sharing_unregister(irq_lines_ptr(), irq_line, context_id, &g_irq_ops);
    ksync_spinlock_unlock(&g_irq_lines_lock);
    return rc;
}

/* Teardown hook for a dying context: drops its routes on every line and forgives
 * the acks it owed, so a line whose only remaining sharers have acked is unmasked
 * rather than left dead. Idempotent, and silent when the context held no line.
 * Takes g_irq_lines_lock. */
void x86_irq_release_context(uint32_t context_id) {
    ksync_spinlock_lock(&g_irq_lines_lock);
    irq_sharing_release_context(irq_lines_ptr(), IRQ_COUNT, context_id, &g_irq_ops);
    ksync_spinlock_unlock(&g_irq_lines_lock);
}

/* Common device-IRQ body, called from the isr_irq_* stubs with the raw vector
 * number (not the line). Runs in interrupt context with IF clear.
 *
 * A vector outside IRQ_VECTOR_BASE..+IRQ_COUNT returns immediately and sends no
 * EOI, which is deliberate: nothing in the controller is pending for it. In PIC
 * modes a spurious IRQ 7/15 also returns early, sending the master-only EOI that
 * a spurious 15 needs and none at all for a spurious 7.
 *
 * Otherwise it does the smallest safe amount of work: timer accounting for line
 * 0, the per-tick sharing maintenance pass, the masked dispatch to registered
 * sharers, and finally the EOI appropriate to the delivery path. Takes
 * g_irq_lines_lock around the sharing calls only, so the EOI is issued unlocked. */
void x86_irq_handler(uint64_t vector) {
    if (vector < IRQ_VECTOR_BASE || vector >= (IRQ_VECTOR_BASE + IRQ_COUNT)) {
        return;
    }

    uint32_t irq_line = (uint32_t)(vector - IRQ_VECTOR_BASE);
#if WASMOS_IRQ_MODE <= 1
    /* Spurious IRQ check applies to both PIC-direct (mode 0) and PIC-via-
     * LINT0-ExtINT (mode 1) because both use the 8259 for device IRQs. */
    if (pic_is_spurious(irq_line)) {
        if (irq_line == 15) {
            /* Spurious IRQ 15: slave never set ISR, but master did latch the
             * cascade line, so master needs EOI; slave must not receive one. */
            outb(PIC1_CMD, PIC_EOI);
        }
        /* Spurious IRQ 7: PIC1 never set ISR — no EOI at all. */
        return;
    }
#endif

    /* IRQ0 is special because it drives scheduler accounting before any routed
     * notification endpoints are serviced. */
    if (irq_line == 0) {
        timer_handle_irq();
    }
    ksync_spinlock_lock(&g_irq_lines_lock);
    if (irq_line == 0) {
        irq_sharing_tick(irq_lines_ptr(), IRQ_COUNT, &g_irq_ops);
    }
    irq_sharing_dispatch(irq_lines_ptr(), irq_line, &g_irq_ops);
    ksync_spinlock_unlock(&g_irq_lines_lock);
    irq_send_eoi(irq_line);
}

/* IRQ 0 handler, called from isr_irq_0 with a borrowed pointer to the register
 * frame the stub built. Runs on the IST1 stack with IF clear.
 *
 * Performs the normal IRQ 0 work (tick accounting, sharing maintenance, EOI) and
 * then hands the frame to process_preempt_from_irq(), which is allowed to rewrite
 * the interrupted rip/cs/rsp/ss in place so the iretq lands in
 * process_preempt_trampoline instead of the interrupted code. That rewrite is the
 * ONLY frame mutation the stub tolerates; any other difference is treated as
 * corruption and panics. RFLAGS in particular must be left alone. */
void x86_timer_irq_handler(irq_frame_t* frame) {
    static uint8_t logged;
    if (WASMOS_TRACE && !logged) {
        logged = 1;
        trace_write("[irq] frame ptr=");
        trace_do(serial_write_hex64(addr_cast(uint64_t, frame)));
        if (frame) {
            trace_write("[irq] frame rip=");
            trace_do(serial_write_hex64(frame->rip));
            trace_write("[irq] frame cs=");
            trace_do(serial_write_hex64(frame->cs));
            trace_write("[irq] frame rflags=");
            trace_do(serial_write_hex64(frame->rflags));
        }
    }
    /* The common IRQ handler performs accounting and EOI; the scheduler-facing
     * preemption handoff is a second, explicit step. */
    x86_irq_handler(IRQ_VECTOR_BASE);
    process_preempt_from_irq(frame);
}
