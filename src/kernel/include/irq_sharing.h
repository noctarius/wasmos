/* irq_sharing.h - Per-line IRQ handler bookkeeping, independent of the
 * interrupt controller.
 *
 * PCI INTx is wire-OR'd: the chipset routes several devices onto one input, so a
 * line can have several handlers ("sharers") and the kernel cannot tell which
 * device asserted — every sharer must inspect its own device. ISA lines
 * (keyboard 1, mouse 12, COM1 4, RTC 8) have a single sharer.
 *
 * The rules this module owns:
 *   - Registering ADDS a sharer; it never replaces one.
 *   - A dispatched line stays masked until EVERY sharer has acked, because
 *     unmasking while one sharer has not yet read its device re-fires the
 *     still-asserted line immediately.
 *   - Acks outstanding past a deadline are force-completed, so one wedged driver
 *     cannot disable a shared device for its co-sharers.
 *   - A per-tick dispatch budget bounds a line whose assertion no sharer clears
 *     (which would otherwise livelock a single-CPU system).
 *
 * Hardware effects (mask/unmask), message delivery and the tick source are
 * injected as ops, and the line table is passed in by the caller, so no state or
 * platform dependency is hidden here. See docs/architecture/05-x86-cpu-architecture.md
 * §Interrupt Controller and IRQ Routing. */
#ifndef WASMOS_IRQ_SHARING_H
#define WASMOS_IRQ_SHARING_H

#include <stdint.h>

/* Handlers allowed on one line. */
#define IRQ_SHARERS_MAX 4
/* Ticks a sharer may take to ack before the line is reopened without it. */
#define IRQ_ACK_DEADLINE_TICKS 50
/* Dispatches one line may cause per tick. */
#define IRQ_DISPATCH_BUDGET_PER_TICK 64u

typedef struct {
    uint8_t in_use;
    uint8_t ack_pending; /* dispatched to this sharer, awaiting its ack */
    uint32_t owner_context_id;
    uint32_t endpoint;
} irq_sharer_t;

typedef struct {
    uint64_t ack_deadline; /* tick by which acks must land; 0 = none outstanding */
    uint64_t budget_tick;  /* tick the dispatch budget below belongs to */
    uint32_t budget_used;  /* dispatches already spent in budget_tick */
    irq_sharer_t sharers[IRQ_SHARERS_MAX];
    uint8_t sharer_count;
    uint8_t pending_acks;    /* line reopens when this reaches 0 */
    uint8_t throttled;       /* dispatch budget for this tick is spent */
    uint8_t throttle_logged; /* rate-limit the throttle diagnostic to once */
} irq_line_t;

typedef struct {
    void (*mask)(uint32_t line);
    void (*unmask)(uint32_t line);
    /* Deliver the IRQ event to one sharer; returns 0 when it was queued. */
    int (*deliver)(uint32_t endpoint, uint32_t line);
    uint64_t (*now_ticks)(void);
    void (*log_throttle)(uint32_t line);
} irq_sharing_ops_t;

/* `line_count` lines are reset to "no sharers". */
void irq_sharing_init(irq_line_t* lines, uint32_t line_count);

/* Add `context_id` as a sharer of `line`, or update its endpoint if already one.
 * Returns 0, or -1 when the line already holds IRQ_SHARERS_MAX sharers. */
int irq_sharing_register(irq_line_t* lines, uint32_t line, uint32_t context_id, uint32_t endpoint,
                         const irq_sharing_ops_t* ops);

/* Report that `context_id` has inspected its device. Returns 0 (including when
 * nothing was outstanding — drivers may ack defensively), -1 when the caller is
 * not a sharer of the line. */
int irq_sharing_ack(irq_line_t* lines, uint32_t line, uint32_t context_id,
                    const irq_sharing_ops_t* ops);

/* Remove one sharer, forgiving any ack it owed. Returns 0, or -1 if not a sharer. */
int irq_sharing_unregister(irq_line_t* lines, uint32_t line, uint32_t context_id,
                           const irq_sharing_ops_t* ops);

/* Remove every sharer of `line`. Returns 0 if any were removed, else -1. */
int irq_sharing_unregister_all(irq_line_t* lines, uint32_t line, const irq_sharing_ops_t* ops);

/* Drop every route held by a dying context across all lines. */
void irq_sharing_release_context(irq_line_t* lines, uint32_t line_count, uint32_t context_id,
                                 const irq_sharing_ops_t* ops);

/* Mask the line and deliver to every sharer, arming the ack deadline. Does
 * nothing when the line has no sharers or is throttled. */
void irq_sharing_dispatch(irq_line_t* lines, uint32_t line, const irq_sharing_ops_t* ops);

/* Per-tick maintenance: force-complete acks past their deadline and release
 * throttled lines. */
void irq_sharing_tick(irq_line_t* lines, uint32_t line_count, const irq_sharing_ops_t* ops);

/* True when the line has at least one registered sharer. */
int irq_sharing_has_sharers(const irq_line_t* lines, uint32_t line);

#endif
