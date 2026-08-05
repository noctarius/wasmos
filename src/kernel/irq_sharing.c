/* irq_sharing.c - Per-line IRQ handler bookkeeping. See irq_sharing.h for the
 * rules and why they exist. Pure logic: the line table and every side effect
 * (mask/unmask, delivery, tick source, logging) are supplied by the caller, so
 * this compiles and is unit-testable on the host. */
#include "irq_sharing.h"

/* Line 0 is the timer: it drives scheduler accounting and is never masked. */
#define IRQ_TIMER_LINE 0u

static irq_sharer_t* sharer_of(irq_line_t* line, uint32_t context_id) {
    for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
        if (line->sharers[i].in_use && line->sharers[i].owner_context_id == context_id) {
            return &line->sharers[i];
        }
    }
    return 0;
}

static void line_reset(irq_line_t* line) {
    for (uint32_t s = 0; s < IRQ_SHARERS_MAX; ++s) {
        line->sharers[s].in_use = 0;
        line->sharers[s].ack_pending = 0;
        line->sharers[s].owner_context_id = 0;
        line->sharers[s].endpoint = 0;
    }
    line->sharer_count = 0;
    line->pending_acks = 0;
    line->throttled = 0;
    line->throttle_logged = 0;
    line->ack_deadline = 0;
    line->budget_tick = 0;
    line->budget_used = 0;
}

static void unmask_if_open(irq_line_t* line, uint32_t irq_line, const irq_sharing_ops_t* ops) {
    if (line->throttled || irq_line == IRQ_TIMER_LINE) {
        return;
    }
    if (ops && ops->unmask) {
        ops->unmask(irq_line);
    }
}

/* Drop one owed ack; reopen the line once the last sharer has reported. */
static void complete_ack(irq_line_t* line, uint32_t irq_line, const irq_sharing_ops_t* ops) {
    if (line->pending_acks == 0) {
        return;
    }
    line->pending_acks--;
    if (line->pending_acks != 0) {
        return;
    }
    line->ack_deadline = 0;
    unmask_if_open(line, irq_line, ops);
}

void irq_sharing_init(irq_line_t* lines, uint32_t line_count) {
    if (!lines) {
        return;
    }
    for (uint32_t i = 0; i < line_count; ++i) {
        line_reset(&lines[i]);
    }
}

int irq_sharing_register(irq_line_t* lines, uint32_t line, uint32_t context_id, uint32_t endpoint,
                         const irq_sharing_ops_t* ops) {
    if (!lines) {
        return -1;
    }
    irq_line_t* l = &lines[line];
    irq_sharer_t* slot = sharer_of(l, context_id);
    if (!slot) {
        for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
            if (!l->sharers[i].in_use) {
                slot = &l->sharers[i];
                break;
            }
        }
        if (!slot) {
            return -1; /* line is full */
        }
        slot->in_use = 1;
        slot->ack_pending = 0;
        l->sharer_count++;
    }
    slot->owner_context_id = context_id;
    slot->endpoint = endpoint;
    /* A late registration must not reopen a line an earlier sharer has not
     * finished with. */
    if (l->pending_acks == 0) {
        unmask_if_open(l, line, ops);
    }
    return 0;
}

int irq_sharing_ack(irq_line_t* lines, uint32_t line, uint32_t context_id,
                    const irq_sharing_ops_t* ops) {
    if (!lines) {
        return -1;
    }
    irq_line_t* l = &lines[line];
    irq_sharer_t* slot = sharer_of(l, context_id);
    if (!slot) {
        return -1;
    }
    if (!slot->ack_pending) {
        return 0; /* nothing outstanding: a defensive or late ack */
    }
    slot->ack_pending = 0;
    complete_ack(l, line, ops);
    return 0;
}

static void drop_sharer(irq_line_t* l, uint32_t line, irq_sharer_t* slot,
                        const irq_sharing_ops_t* ops) {
    uint8_t owed = slot->ack_pending;
    slot->in_use = 0;
    slot->ack_pending = 0;
    slot->owner_context_id = 0;
    slot->endpoint = 0;
    if (l->sharer_count > 0) {
        l->sharer_count--;
    }
    /* Forgive the ack so a departing (or dead) sharer cannot leave the line
     * masked for the others. */
    if (owed) {
        complete_ack(l, line, ops);
    }
    if (l->sharer_count == 0) {
        l->pending_acks = 0;
        l->ack_deadline = 0;
        if (line != IRQ_TIMER_LINE && ops && ops->mask) {
            ops->mask(line);
        }
    }
}

int irq_sharing_unregister(irq_line_t* lines, uint32_t line, uint32_t context_id,
                           const irq_sharing_ops_t* ops) {
    if (!lines) {
        return -1;
    }
    irq_line_t* l = &lines[line];
    irq_sharer_t* slot = sharer_of(l, context_id);
    if (!slot) {
        return -1;
    }
    drop_sharer(l, line, slot, ops);
    return 0;
}

int irq_sharing_unregister_all(irq_line_t* lines, uint32_t line, const irq_sharing_ops_t* ops) {
    if (!lines) {
        return -1;
    }
    irq_line_t* l = &lines[line];
    int found = -1;
    for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
        if (l->sharers[i].in_use) {
            drop_sharer(l, line, &l->sharers[i], ops);
            found = 0;
        }
    }
    return found;
}

void irq_sharing_release_context(irq_line_t* lines, uint32_t line_count, uint32_t context_id,
                                 const irq_sharing_ops_t* ops) {
    if (!lines) {
        return;
    }
    for (uint32_t line = 0; line < line_count; ++line) {
        irq_sharer_t* slot = sharer_of(&lines[line], context_id);
        if (slot) {
            drop_sharer(&lines[line], line, slot, ops);
        }
    }
}

void irq_sharing_dispatch(irq_line_t* lines, uint32_t line, const irq_sharing_ops_t* ops) {
    if (!lines || !ops) {
        return;
    }
    irq_line_t* l = &lines[line];
    if (l->sharer_count == 0 || l->throttled) {
        return;
    }
    if (line != IRQ_TIMER_LINE && ops->mask) {
        ops->mask(line);
    }

    uint64_t now = ops->now_ticks ? ops->now_ticks() : 0;
    if (l->budget_tick != now) {
        l->budget_tick = now;
        l->budget_used = 0;
    }
    if (++l->budget_used > IRQ_DISPATCH_BUDGET_PER_TICK) {
        /* Nobody is clearing this assertion fast enough (or at all): stop
         * dispatching it for the rest of the tick rather than let it consume the
         * machine.  irq_sharing_tick() reopens the line. */
        l->throttled = 1;
        if (!l->throttle_logged) {
            l->throttle_logged = 1;
            if (ops->log_throttle) {
                ops->log_throttle(line);
            }
        }
        return;
    }

    l->pending_acks = 0;
    for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
        irq_sharer_t* slot = &l->sharers[i];
        if (!slot->in_use) {
            continue;
        }
        /* A full endpoint queue must not make the line wait for an ack that will
         * never come: skip that sharer for this dispatch. */
        if (ops->deliver && ops->deliver(slot->endpoint, line) == 0) {
            slot->ack_pending = 1;
            l->pending_acks++;
        }
    }
    if (l->pending_acks == 0) {
        /* Nobody was reachable — reopen rather than strand the line. */
        unmask_if_open(l, line, ops);
        return;
    }
    l->ack_deadline = now + IRQ_ACK_DEADLINE_TICKS;
}

void irq_sharing_tick(irq_line_t* lines, uint32_t line_count, const irq_sharing_ops_t* ops) {
    if (!lines || !ops) {
        return;
    }
    uint64_t now = ops->now_ticks ? ops->now_ticks() : 0;
    for (uint32_t line = 0; line < line_count; ++line) {
        irq_line_t* l = &lines[line];
        if (l->pending_acks > 0 && l->ack_deadline != 0 && now >= l->ack_deadline) {
            /* One wedged driver must not disable a shared device for the others. */
            for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
                l->sharers[i].ack_pending = 0;
            }
            l->pending_acks = 0;
            l->ack_deadline = 0;
            unmask_if_open(l, line, ops);
        }
        if (l->throttled) {
            l->throttled = 0;
            l->budget_tick = now;
            l->budget_used = 0;
            if (l->pending_acks == 0 && l->sharer_count > 0) {
                unmask_if_open(l, line, ops);
            }
        }
    }
}

int irq_sharing_has_sharers(const irq_line_t* lines, uint32_t line) {
    return (lines && lines[line].sharer_count > 0) ? 1 : 0;
}
