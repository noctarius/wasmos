#include "irq.h"
#include "arch/x86_64/irq_x86_64.h"
#include "ipc.h"
#include "serial.h"
#include "kpanic.h"
#include "timer.h"
#include "policy.h"
#include "paging.h"
#if WASMOS_IRQ_MODE >= 1
#include "arch/x86_64/lapic.h"
#endif
#if WASMOS_IRQ_MODE == 2
#include "arch/x86_64/ioapic.h"
#endif

/*
 * irq.c handles PIC setup, IRQ routing, and the minimal interrupt-side work
 * needed to wake the rest of the system. The policy rule is strict: do the
 * smallest safe amount of work in interrupt context, then let the scheduler and
 * regular kernel paths finish the job.
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

/* One registered handler on a line.  PCI INTx is wire-OR'd: the chipset routes
 * several devices onto one input, so a line can have several sharers and the
 * kernel cannot tell which device asserted — every sharer must look at its own
 * device.  ISA lines (keyboard 1, mouse 12, COM1 4, RTC 8) have a single sharer. */
typedef struct {
    uint8_t in_use;
    uint8_t ack_pending; /* dispatched to this sharer, awaiting its irq_ack */
    uint32_t owner_context_id;
    uint32_t endpoint; /* message endpoint: IRQ delivered as IPC_IRQ_EVENT_TYPE msg */
} irq_sharer_t;

/* Per-line dispatch state.  The line stays masked from dispatch until *every*
 * sharer has acked, because unmasking while one sharer has not yet read its
 * device re-fires the still-asserted line immediately. */
typedef struct {
    uint64_t ack_deadline; /* tick by which acks must land; 0 = none outstanding */
    uint64_t budget_tick;  /* tick the dispatch budget below belongs to */
    uint32_t budget_used;  /* dispatches already spent in budget_tick */
    irq_sharer_t sharers[IRQ_SHARERS_MAX];
    uint8_t sharer_count;
    uint8_t pending_acks;    /* unmask when this reaches 0 */
    uint8_t throttled;       /* dispatch budget for this tick is spent */
    uint8_t throttle_logged; /* rate-limit the throttle diagnostic to once */
} irq_line_t;

static irq_line_t g_irq_lines[IRQ_COUNT];
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

/* Find the slot belonging to context_id on this line, or NULL. */
static irq_sharer_t* irq_sharer_of(irq_line_t* line, uint32_t context_id) {
    for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
        if (line->sharers[i].in_use && line->sharers[i].owner_context_id == context_id) {
            return &line->sharers[i];
        }
    }
    return 0;
}

/* Drop one owed ack.  Unmasks the line once the last sharer has reported, unless
 * the line is throttled (the tick handler unmasks those). */
static void irq_complete_ack(uint32_t irq_line, irq_line_t* line) {
    if (line->pending_acks == 0) {
        return;
    }
    line->pending_acks--;
    if (line->pending_acks != 0) {
        return;
    }
    line->ack_deadline = 0;
    if (!line->throttled && irq_line != 0) {
        x86_irq_unmask(irq_line);
    }
}

#if WASMOS_IRQ_MODE <= 1
static inline uint8_t* pic_mask1_slot(void) {
    return (uint8_t*)(void*)irq_alias_ptr((uintptr_t)&g_pic_mask1);
}

static inline uint8_t* pic_mask2_slot(void) {
    return (uint8_t*)(void*)irq_alias_ptr((uintptr_t)&g_pic_mask2);
}
#endif

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
                  (unsigned long long)saved[0], (unsigned long long)saved[1],
                  (unsigned long long)saved[2], (unsigned long long)current[0],
                  (unsigned long long)current[1], (unsigned long long)current[2]);
    /* Corrupt return frame — unrecoverable. a=saved rip, b=current rip. */
    kpanic("irq_iret_frame_corrupt", saved[0], current[0]);
}

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
     * SDM Vol 3A §10.8.4).  Only the 8259 PIC needs an EOI to clear its ISR.
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

void x86_irq_init(void) {
    irq_line_t* lines = irq_lines_ptr();
    for (uint32_t i = 0; i < IRQ_COUNT; ++i) {
        irq_line_t* line = &lines[i];
        for (uint32_t s = 0; s < IRQ_SHARERS_MAX; ++s) {
            line->sharers[s].in_use = 0;
            line->sharers[s].ack_pending = 0;
            line->sharers[s].owner_context_id = 0;
            line->sharers[s].endpoint = IPC_ENDPOINT_NONE;
        }
        line->sharer_count = 0;
        line->pending_acks = 0;
        line->throttled = 0;
        line->throttle_logged = 0;
        line->ack_deadline = 0;
        line->budget_tick = 0;
        line->budget_used = 0;
    }

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
     * lines we explicitly unmask later become active. */
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
 * per-line polarity control, so this is a no-op there. */
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

void x86_irq_late_init(const boot_info_t* boot_info) {
#if WASMOS_IRQ_MODE == 2
    ioapic_init(boot_info);
#else
    (void)boot_info;
#endif
}

int x86_irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    irq_line_t* lines = irq_lines_ptr();
    if (irq_line >= IRQ_COUNT || endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (policy_authorize(context_id, POLICY_ACTION_IRQ_ROUTE, irq_line) != 0) {
        return -1;
    }

    uint32_t owner_context_id = 0;
    if (ipc_endpoint_owner(endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id != context_id) {
        return -1;
    }

    irq_line_t* line = &lines[irq_line];
    /* Re-registration by the same context updates its endpoint in place; a
     * different context is ADDED as a sharer rather than replacing the first
     * (replacing silently stole the line and stopped the original driver's
     * interrupts). */
    irq_sharer_t* slot = irq_sharer_of(line, context_id);
    if (!slot) {
        for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
            if (!line->sharers[i].in_use) {
                slot = &line->sharers[i];
                break;
            }
        }
        if (!slot) {
            return -1; /* line already has IRQ_SHARERS_MAX handlers */
        }
        slot->in_use = 1;
        slot->ack_pending = 0;
        line->sharer_count++;
    }
    slot->owner_context_id = context_id;
    slot->endpoint = endpoint;
    /* Only the first sharer needs to open the line; a later one must not unmask
     * while an earlier sharer still owes an ack. */
    if (line->pending_acks == 0 && !line->throttled) {
        x86_irq_unmask(irq_line);
    }
    return 0;
}

int x86_irq_ack(uint32_t context_id, uint32_t irq_line) {
    irq_line_t* lines = irq_lines_ptr();
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
    irq_line_t* line = &lines[irq_line];
    irq_sharer_t* slot = irq_sharer_of(line, context_id);
    if (!slot) {
        return -1;
    }
    /* Acking with nothing outstanding is a harmless no-op: drivers may ack
     * defensively, and a forced deadline completion may already have cleared it. */
    if (!slot->ack_pending) {
        return 0;
    }
    slot->ack_pending = 0;
    irq_complete_ack(irq_line, line);
    return 0;
}

/* Remove one sharer.  Forgives any ack it owed so a departing (or dead) driver
 * cannot leave the line masked for the others, and masks only once the last
 * sharer is gone. */
static int irq_drop_sharer(irq_line_t* line, uint32_t irq_line, irq_sharer_t* slot) {
    uint8_t owed = slot->ack_pending;
    slot->in_use = 0;
    slot->ack_pending = 0;
    slot->owner_context_id = 0;
    slot->endpoint = IPC_ENDPOINT_NONE;
    if (line->sharer_count > 0) {
        line->sharer_count--;
    }
    if (owed) {
        irq_complete_ack(irq_line, line);
    }
    if (line->sharer_count == 0) {
        line->pending_acks = 0;
        line->ack_deadline = 0;
        x86_irq_mask(irq_line);
    }
    return 0;
}

int x86_irq_unregister(uint32_t context_id, uint32_t irq_line) {
    irq_line_t* lines = irq_lines_ptr();
    if (irq_line >= IRQ_COUNT) {
        return -1;
    }
    irq_line_t* line = &lines[irq_line];
    if (context_id == IPC_CONTEXT_KERNEL) {
        int found = -1;
        for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
            if (line->sharers[i].in_use) {
                (void)irq_drop_sharer(line, irq_line, &line->sharers[i]);
                found = 0;
            }
        }
        return found;
    }
    irq_sharer_t* slot = irq_sharer_of(line, context_id);
    if (!slot) {
        return -1;
    }
    return irq_drop_sharer(line, irq_line, slot);
}

/* Release every route held by a dying context.  Without this a reaped driver
 * left its slot in_use pointing at a dead endpoint, and any ack it owed kept the
 * line masked forever. */
void x86_irq_release_context(uint32_t context_id) {
    irq_line_t* lines = irq_lines_ptr();
    for (uint32_t l = 0; l < IRQ_COUNT; ++l) {
        irq_sharer_t* slot = irq_sharer_of(&lines[l], context_id);
        if (slot) {
            (void)irq_drop_sharer(&lines[l], l, slot);
        }
    }
}

/* Timer-tick maintenance, run from the IRQ0 path.
 *
 * (1) Ack deadline: one wedged or slow driver must not disable a shared device
 *     for its co-sharers, so outstanding acks are force-completed once the
 *     deadline passes and the line reopens.
 * (2) Throttle release: a line whose dispatch budget was spent gets it back. */
static void irq_tick_scan(irq_line_t* lines) {
    uint64_t now = timer_ticks();
    for (uint32_t l = 0; l < IRQ_COUNT; ++l) {
        irq_line_t* line = &lines[l];
        if (line->pending_acks > 0 && line->ack_deadline != 0 && now >= line->ack_deadline) {
            for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
                line->sharers[i].ack_pending = 0;
            }
            line->pending_acks = 0;
            line->ack_deadline = 0;
            if (!line->throttled && l != 0) {
                x86_irq_unmask(l);
            }
        }
        if (line->throttled) {
            line->throttled = 0;
            line->budget_tick = now;
            line->budget_used = 0;
            if (line->pending_acks == 0 && line->sharer_count > 0 && l != 0) {
                x86_irq_unmask(l);
            }
        }
    }
}

void x86_irq_handler(uint64_t vector) {
    irq_line_t* lines = irq_lines_ptr();
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

    irq_line_t* line = &lines[irq_line];
    /* IRQ0 is special because it drives scheduler accounting before any routed
     * notification endpoints are serviced. */
    if (irq_line == 0) {
        timer_handle_irq();
        irq_tick_scan(lines);
    }
    if (line->sharer_count > 0 && !line->throttled) {
        /* Mask before sending, and keep the line masked until EVERY sharer has
         * acked: the line stays asserted until each driver reads its own device
         * register (i8042 OBF, virtio ISR, ...), so unmasking after the first ack
         * re-fires the interrupt immediately and floods the endpoint queues. */
        if (irq_line != 0) {
            x86_irq_mask(irq_line);
        }
        /* Bound the work one line can cause per tick.  A device asserting a line
         * nobody clears would otherwise re-fire on every unmask and livelock a
         * single-CPU system; throttling converts that into bounded overhead plus a
         * diagnostic, and the tick scan re-opens the line. */
        uint64_t now = timer_ticks();
        if (line->budget_tick != now) {
            line->budget_tick = now;
            line->budget_used = 0;
        }
        if (++line->budget_used > IRQ_DISPATCH_BUDGET_PER_TICK) {
            line->throttled = 1;
            if (!line->throttle_logged) {
                line->throttle_logged = 1;
                /* serial_write_hex64 terminates the line, so the number goes
                 * last to keep the diagnostic on one line. */
                serial_write("[irq] dispatch budget exhausted (unclaimed assertion or runaway"
                             " device), throttling line=");
                serial_write_hex64(irq_line);
            }
            irq_send_eoi(irq_line);
            return;
        }

        line->pending_acks = 0;
        for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
            irq_sharer_t* slot = &line->sharers[i];
            if (!slot->in_use) {
                continue;
            }
            ipc_message_t irq_msg;
            irq_msg.type = IPC_IRQ_EVENT_TYPE;
            irq_msg.request_id = (int32_t)irq_line;
            irq_msg.source = IPC_ENDPOINT_NONE;
            irq_msg.destination = slot->endpoint;
            irq_msg.arg0 = (int32_t)irq_line;
            irq_msg.arg1 = 0;
            irq_msg.arg2 = 0;
            irq_msg.arg3 = 0;
            /* A full endpoint queue must not make the line wait for an ack that
             * will never come: skip that sharer for this dispatch. */
            if (ipc_send_from(IPC_CONTEXT_KERNEL, slot->endpoint, &irq_msg) == IPC_OK) {
                slot->ack_pending = 1;
                line->pending_acks++;
            }
        }
        if (line->pending_acks == 0) {
            /* Nobody was reachable — reopen the line rather than stranding it. */
            if (irq_line != 0) {
                x86_irq_unmask(irq_line);
            }
        } else {
            line->ack_deadline = timer_ticks() + IRQ_ACK_DEADLINE_TICKS;
        }
    }
    irq_send_eoi(irq_line);
}

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
