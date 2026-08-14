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

/* Resets every line in the caller's table to "no sharers, nothing owed, not
 * throttled".  It issues no mask/unmask, so the interrupt controller's actual
 * state is unchanged and the caller must bring the two into agreement.  A NULL
 * table is ignored.
 *
 * `lines` is borrowed for the call; the whole module keeps no state of its own,
 * which is what makes it host-testable. */
void irq_sharing_init(irq_line_t* lines, uint32_t line_count) {
    if (!lines) {
        return;
    }
    for (uint32_t i = 0; i < line_count; ++i) {
        line_reset(&lines[i]);
    }
}

/* Adds context_id as a sharer of `line`, or updates its endpoint if it is
 * already one — re-registering is not an error and does not consume a second
 * slot.
 *
 * The line is unmasked as a side effect, but only when nothing is currently owed
 * on it and it is not the timer line, so a late arrival cannot reopen a line an
 * earlier sharer has not acked.
 *
 * `line` is used as an unchecked index into `lines`: this function and the rest
 * of the family take no line_count, so the CALLER must bound it.  Returns 0 on
 * success, WASMOS_ERR_IRQ_BAD_LINE for a NULL table, and
 * WASMOS_ERR_IRQ_LINE_FULL when all IRQ_SHARERS_MAX slots are taken. */
int irq_sharing_register(irq_line_t* lines, uint32_t line, uint32_t context_id, uint32_t endpoint,
                         const irq_sharing_ops_t* ops) {
    if (!lines) {
        return WASMOS_ERR_IRQ_BAD_LINE;
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
            return WASMOS_ERR_IRQ_LINE_FULL;
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

/* Clears one sharer's outstanding ack.  The line is unmasked only when this was
 * the LAST ack owed, so a shared line stays masked until every driver has
 * reported — that is the whole point of the ack accounting.
 *
 * An ack from a sharer with nothing outstanding returns 0 and changes nothing,
 * so a duplicate or late ack cannot reopen the line early.  Returns
 * WASMOS_ERR_IRQ_BAD_LINE for a NULL table and WASMOS_ERR_IRQ_NOT_A_SHARER when
 * the context is not registered on this line. */
int irq_sharing_ack(irq_line_t* lines, uint32_t line, uint32_t context_id,
                    const irq_sharing_ops_t* ops) {
    if (!lines) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    irq_line_t* l = &lines[line];
    irq_sharer_t* slot = sharer_of(l, context_id);
    if (!slot) {
        return WASMOS_ERR_IRQ_NOT_A_SHARER;
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

/* Removes one sharer.  Any ack it still owed is forgiven, so a dying driver
 * cannot leave the line masked for the others, and the line is MASKED when the
 * last sharer leaves (except the timer line, which is never masked).
 *
 * Returns 0 on success, WASMOS_ERR_IRQ_BAD_LINE for a NULL table, and
 * WASMOS_ERR_IRQ_NOT_A_SHARER when the context is not registered here. */
int irq_sharing_unregister(irq_line_t* lines, uint32_t line, uint32_t context_id,
                           const irq_sharing_ops_t* ops) {
    if (!lines) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    irq_line_t* l = &lines[line];
    irq_sharer_t* slot = sharer_of(l, context_id);
    if (!slot) {
        return WASMOS_ERR_IRQ_NOT_A_SHARER;
    }
    drop_sharer(l, line, slot, ops);
    return 0;
}

/* Drops every sharer of one line, with the same forgiveness and end-state
 * masking as irq_sharing_unregister.  Returns 0 when at least one sharer was
 * removed, WASMOS_ERR_IRQ_NOT_A_SHARER when the line had none, and
 * WASMOS_ERR_IRQ_BAD_LINE for a NULL table. */
int irq_sharing_unregister_all(irq_line_t* lines, uint32_t line, const irq_sharing_ops_t* ops) {
    if (!lines) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    irq_line_t* l = &lines[line];
    int found = WASMOS_ERR_IRQ_NOT_A_SHARER;
    for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
        if (l->sharers[i].in_use) {
            drop_sharer(l, line, &l->sharers[i], ops);
            found = 0;
        }
    }
    return found;
}

/* Sweeps a dying context off every line in the table, dropping at most one
 * sharer slot per line.  Reports nothing: a context that shared no line is a
 * no-op, which is the expected case for most teardowns.  Unlike the rest of the
 * family this one IS given line_count and stays inside it. */
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

/* Fans one hardware interrupt out to every sharer of the line and records how
 * many acks are now owed.  Called from the ISR, so it must not block.
 *
 * Sequence: mask the line (never the timer line), charge the per-tick dispatch
 * budget, deliver to each sharer, and arm an ack deadline
 * IRQ_ACK_DEADLINE_TICKS out.  A sharer whose delivery fails — typically a full
 * endpoint queue — is skipped and owes nothing, so an unreachable driver cannot
 * strand the line; if that leaves nobody owing, the line is reopened at once.
 *
 * Exceeding IRQ_DISPATCH_BUDGET_PER_TICK dispatches in one tick throttles the
 * line: it stays masked and is skipped until irq_sharing_tick reopens it, and
 * the throttle is logged once per episode.  A line with no sharers, or one
 * already throttled, dispatches nothing.
 *
 * Reports nothing; ops must be non-NULL or the call is a no-op. */
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

/* Periodic recovery pass over the whole table, expected once per timer tick.
 *
 * Two jobs.  A line whose ack deadline has passed has ALL outstanding acks
 * forgiven and is reopened, so one wedged driver cannot disable a shared device
 * for the rest — the wedged driver's later ack then finds nothing outstanding
 * and is ignored.  A throttled line has its budget reset and is reopened, unless
 * it is now waiting on acks or has lost all its sharers.
 *
 * Reports nothing.  A NULL table or NULL ops makes it a no-op, which also means
 * throttled lines are never reopened without an ops. */
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

/* 1 when the line has at least one registered sharer, 0 otherwise or for a NULL
 * table.  `line` is again an unchecked index. */
int irq_sharing_has_sharers(const irq_line_t* lines, uint32_t line) {
    return (lines && lines[line].sharer_count > 0) ? 1 : 0;
}
