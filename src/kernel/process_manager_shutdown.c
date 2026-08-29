/* process_manager_shutdown.c - the orderly shutdown sequence.
 *
 * Design: docs/architecture/15-drivers-and-services.md, "Orderly Shutdown
 * Design Direction". Shutdown is the counterpart of the readiness handshake:
 * startup is spawn -> PROC_IPC_NOTIFY_READY -> the spawner unblocks; shutdown is
 * WASMOS_IPC_SHUTDOWN_REQ -> the participant quiesces -> WASMOS_IPC_SHUTDOWN_DONE.
 * The PM owns both because it owns process lifecycle, the service registry and
 * the spawn order.
 *
 * The sequence is a state machine stepped once per PM dispatch rather than a
 * loop, because the replies it waits for arrive on the PM's own endpoint and
 * only the dispatch loop drains it. Blocking inside a step would deadlock
 * against the reply it is blocking for.
 */
#include "process_manager_internal.h"
#include "ipc.h"
#include "klog.h"
#include "process.h"
#include "process_manager.h"
#include "system_control.h"
#include "timer.h"

/* Milliseconds a single participant is given before it is passed over.
 *
 * Bounded and best-effort by design: the sequence exists to let a filesystem
 * record a clean unmount, and a volume that misses it mounts read-only next boot
 * rather than serving inconsistent metadata. Shutdown may therefore give up on a
 * participant; it may not fail to bring the machine down.
 *
 * Sized against the work, not against a round trip. fs-wfs answers by writing
 * its superblock and its backup copies, and every one of those writes ends in a
 * cache flush that a device backed by real media takes real time to honour --
 * flushes measured, on this tree, an order of magnitude slower than the sector
 * transfers the block drivers are otherwise tuned for. Expiry here is not merely
 * a missed opportunity: the next thing the sequence does when the last
 * participant is passed over is cut the power, so a deadline shorter than the
 * write it interrupts turns a clean unmount into a torn superblock -- the one
 * outcome worse than never having run. It costs nothing on the normal path,
 * where the participant answers in milliseconds. */
#ifndef WASMOS_PM_SHUTDOWN_DEADLINE_MS
#define WASMOS_PM_SHUTDOWN_DEADLINE_MS 8000u
#endif

/* Collect the participants: every distinct context owning a registered service
 * endpoint that declared WASMOS_SVC_FLAG_WANTS_SHUTDOWN, newest first.
 *
 * Opt-in, because the sequence is sequential. A participant may need the
 * services beneath it while it quiesces, so they are notified one at a time and
 * each costs its deadline; a participant with nothing to persist would spend
 * that deadline to say so. Declaring the flag is how a service says the
 * distinction matters for it. The registry is the right place to read it from:
 * it is also what supplies the endpoint to send to.
 *
 * `pid` is monotonic and never reused within a boot, so descending pid IS
 * reverse spawn order (§ the design doc): fs-wfs comes down before the block
 * driver beneath it. Sorted by insertion, which is O(n^2) over a table that
 * holds tens of entries and needs no allocation.
 *
 * A context that registered several names -- a driver claiming both a name and a
 * class -- is collected once. Notifying it twice would have it quiesce, answer,
 * and then be told again. */
static void pm_shutdown_collect(void) {
    list_iter_t it;
    const pm_service_entry_t* entry = (const pm_service_entry_t*)list_first(&g_pm.services, &it);

    g_pm.shutdown.count = 0;
    g_pm.shutdown.index = 0;
    while (entry) {
        if (entry->in_use && entry->owner_context_id != 0 &&
            (entry->flags & WASMOS_SVC_FLAG_WANTS_SHUTDOWN) != 0) {
            process_t* owner = process_find_by_context(entry->owner_context_id);
            uint32_t i;
            uint32_t at;

            if (!owner || owner->pid == 0) {
                entry = (const pm_service_entry_t*)list_next(&it);
                continue;
            }
            for (i = 0; i < g_pm.shutdown.count; ++i) {
                if (g_pm.shutdown.context_ids[i] == entry->owner_context_id) {
                    break;
                }
            }
            if (i < g_pm.shutdown.count || g_pm.shutdown.count >= WASMOS_PM_SHUTDOWN_MAX) {
                entry = (const pm_service_entry_t*)list_next(&it);
                continue;
            }
            for (at = 0; at < g_pm.shutdown.count; ++at) {
                if (g_pm.shutdown.pids[at] < owner->pid) {
                    break;
                }
            }
            for (i = g_pm.shutdown.count; i > at; --i) {
                g_pm.shutdown.pids[i] = g_pm.shutdown.pids[i - 1];
                g_pm.shutdown.endpoints[i] = g_pm.shutdown.endpoints[i - 1];
                g_pm.shutdown.context_ids[i] = g_pm.shutdown.context_ids[i - 1];
            }
            g_pm.shutdown.pids[at] = owner->pid;
            g_pm.shutdown.endpoints[at] = entry->endpoint;
            g_pm.shutdown.context_ids[at] = entry->owner_context_id;
            g_pm.shutdown.count++;
        }
        entry = (const pm_service_entry_t*)list_next(&it);
    }
}

/* Notify the participant at `index` and arm its deadline.
 *
 * The deadline is armed from the send whether or not the send succeeded, so a
 * participant whose endpoint has gone costs one deadline and is then passed over
 * rather than leaving the sequence with nothing to wait for. */
static void pm_shutdown_notify(uint32_t pm_context_id) {
    ipc_message_t req;
    uint32_t index = g_pm.shutdown.index;

    req.type = WASMOS_IPC_SHUTDOWN_REQ;
    req.source = pm_atomic_load_u32(&g_pm.proc_endpoint);
    req.destination = g_pm.shutdown.endpoints[index];
    req.request_id = 0;
    req.arg0 = g_pm.shutdown.reason;
    req.arg1 = 0;
    req.arg2 = 0;
    req.arg3 = 0;
    (void)ipc_send_from(pm_context_id, g_pm.shutdown.endpoints[index], &req);
    g_pm.shutdown.pending = 1;
    g_pm.shutdown.deadline_ticks =
        timer_ticks() + timer_ms_to_ticks(WASMOS_PM_SHUTDOWN_DEADLINE_MS);
}

void pm_shutdown_note_done(const ipc_message_t* msg) {
    if (!msg || !g_pm.shutdown.active || !g_pm.shutdown.pending) {
        return;
    }
    /* Matched on the SOURCE endpoint, not on a request id: a participant answers
     * from the endpoint it was told on, and a stale DONE from a participant
     * already passed over must not retire the one now outstanding. */
    if (msg->source != g_pm.shutdown.endpoints[g_pm.shutdown.index]) {
        return;
    }
    g_pm.shutdown.pending = 0;
    pm_atomic_store_u32(&g_pm.shutdown.index, g_pm.shutdown.index + 1u);
}

void pm_shutdown_step(uint32_t pm_context_id) {
    if (!g_pm.shutdown.active) {
        if (!pm_atomic_load_u32(&g_pm.shutdown.requested)) {
            return;
        }
        pm_atomic_store_u32(&g_pm.shutdown.active, 1u);
        pm_shutdown_collect();
        klog_write("[pm] shutdown: begin\n");
    }
    if (g_pm.shutdown.pending) {
        if (timer_ticks() < g_pm.shutdown.deadline_ticks) {
            return;
        }
        /* Passed over rather than retried: the deadline has already cost the
         * shutdown its budget for this participant, and a participant that
         * cannot answer once is not more likely to answer twice. */
        klog_write("[pm] shutdown: participant did not answer\n");
        g_pm.shutdown.pending = 0;
        pm_atomic_store_u32(&g_pm.shutdown.index, g_pm.shutdown.index + 1u);
    }
    if (g_pm.shutdown.index < g_pm.shutdown.count) {
        pm_shutdown_notify(pm_context_id);
        return;
    }
    klog_write("[pm] shutdown: complete\n");
    if (g_pm.shutdown.reason == WASMOS_SHUTDOWN_REASON_REBOOT) {
        kernel_system_reboot();
    }
    kernel_system_poweroff();
}

/* How long the halting process parks between checks. Matched to the PM's own
 * idle poll interval, so a step the PM takes is noticed about as fast as it
 * happens. */
#define WASMOS_PM_SHUTDOWN_WAIT_SLICE_MS 50u

/* Bring the machine down for `reason`, giving the participants their sequence
 * first. Runs on the HALTING process's CPU; the sequence itself runs on the PM's.
 *
 * The wait PARKS the caller on an endpoint nothing ever sends to -- created
 * here and never published, so it cannot become readable and turn the park into
 * a poll -- rather than yielding in a loop. On a single-CPU guest the difference is the whole
 * behaviour: a yield loop keeps the halting process runnable, so the scheduler
 * has no reason to run the process manager, and the sequence never starts --
 * measured as the machine powering off through the fallback below with no
 * participant notified. Parking takes the caller out of the ready set entirely.
 *
 * Progress -- not total time -- bounds it. A sequence stepping through many
 * participants at the per-participant deadline legitimately takes a while, so a
 * total budget would either cut a working shutdown short or be too loose to
 * catch a wedged PM. The caller therefore watches how far the sequence has got
 * and gives up only when that has not moved for two deadlines, at which point it
 * powers the machine off itself. Halt always halts.
 *
 * An endpoint that cannot be created costs the shutdown its sequence rather than
 * its halt: there is nothing to park on, and spinning instead is what this
 * exists to avoid. */
void kernel_system_shutdown(uint32_t reason, uint32_t context_id) {
    uint32_t endpoint = IPC_ENDPOINT_NONE;
    uint32_t progress;
    uint64_t give_up;

    if (ipc_endpoint_create(context_id, &endpoint) != IPC_OK) {
        klog_write("[pm] shutdown: no endpoint to wait on, powering off\n");
        if (reason == WASMOS_SHUTDOWN_REASON_REBOOT) {
            kernel_system_reboot();
        }
        kernel_system_poweroff();
    }

    kernel_system_shutdown_arm(reason);
    /* One number that only ever moves forward: 0 until the PM has collected the
     * participants, then one past the index it is waiting on. Combining the two
     * is what makes "the PM never started" a stall like any other -- watching
     * `index` alone cannot distinguish not-yet-started from waiting-on-the-first,
     * and treating not-yet-started as progress would reset this deadline forever
     * on a PM that never runs, which is the one case it exists to catch. */
    progress = 0;
    give_up = timer_ticks() + timer_ms_to_ticks(2u * WASMOS_PM_SHUTDOWN_DEADLINE_MS);
    while (timer_ticks() < give_up) {
        /* Read with the helpers because the PM writes them on its own CPU. The
         * pair collapses to one forward-only number, so a stale read can only
         * delay the give-up reset below, never bring it forward. */
        uint32_t now = pm_atomic_load_u32(&g_pm.shutdown.active)
                           ? (1u + pm_atomic_load_u32(&g_pm.shutdown.index))
                           : 0u;

        if (now != progress) {
            progress = now;
            give_up = timer_ticks() + timer_ms_to_ticks(2u * WASMOS_PM_SHUTDOWN_DEADLINE_MS);
        }
        if (ipc_endpoint_wait_for(context_id, endpoint, WASMOS_PM_SHUTDOWN_WAIT_SLICE_MS) !=
            IPC_OK) {
            /* The wait returned without parking, so repeating it is the spin this
             * function exists to avoid -- and a spin here starves the very
             * process the loop is waiting for. Give the machine up rather than
             * hold the CPU against it. */
            klog_write("[pm] shutdown: cannot park, powering off\n");
            break;
        }
    }
    klog_write("[pm] shutdown: sequence stalled, powering off\n");
    if (reason == WASMOS_SHUTDOWN_REASON_REBOOT) {
        kernel_system_reboot();
    }
    kernel_system_poweroff();
}

/* Armed from the halting process's CPU, stepped on the PM's. `requested` is
 * stored last and with release ordering, which publishes `reason` to the PM's
 * acquiring load; every other field of g_pm.shutdown is written only by
 * pm_shutdown_step, on the PM's own CPU, after it observes this store. */
void kernel_system_shutdown_arm(uint32_t reason) {
    g_pm.shutdown.reason =
        (reason == WASMOS_SHUTDOWN_REASON_REBOOT) ? WASMOS_SHUTDOWN_REASON_REBOOT : 0u;
    pm_atomic_store_u32(&g_pm.shutdown.requested, 1u);
}
