#include "poll.h"
#include "sched_event.h"
#include "ipc.h"
#include "stdlib.h"
#include "string.h"

/*
 * poll.c — push-model readiness notification for the poll-hub.
 *
 * Each IPC endpoint that has watchers carries a lazily-allocated poll_struct_t.
 * When a message arrives, ipc_send_from calls poll_notify(ps, POLL_EV_IN, ep_id),
 * which pushes directly to every registered ipc_select_t — O(watchers), not O(N).
 *
 * Nothing here takes a lock.  A hub is reachable only through the endpoint that
 * owns it, and every entry point below is called with that endpoint's ep->lock
 * held (ipc.c documents the file-wide order: g_select_table_lock ->
 * g_endpoint_table_lock -> ep->lock -> an event lock).  That ep->lock is also
 * what keeps poll_notify from walking watcher nodes an endpoint teardown is
 * freeing concurrently.
 */

#ifdef WASMOS_POLL_TEST_SEAMS
/* How many hubs are outstanding. The hub is reachable only from the endpoint
 * that owns it, and that endpoint drops the pointer as it goes, so its release
 * cannot be observed from outside without this. */
static uint32_t g_poll_live_structs;

uint32_t poll_test_live_structs(void) {
    return g_poll_live_structs;
}
#endif

/* Allocates a zeroed hub (no watchers on any event).  Returns 0 when out of
 * memory; the caller must treat that as a failure to watch, not as "watching
 * with nobody registered".  Ownership transfers to the caller, which in practice
 * means the endpoint stores it in ep->poll_struct and releases it from
 * poll_struct_free at endpoint teardown. */
poll_struct_t* poll_struct_alloc(void) {
    poll_struct_t* ps = (poll_struct_t*)malloc(sizeof(poll_struct_t));
    if (!ps) {
        return 0;
    }
    memset(ps, 0, sizeof(*ps));
#ifdef WASMOS_POLL_TEST_SEAMS
    g_poll_live_structs++;
#endif
    return ps;
}

/* Registers `sel` as a watcher of `ev` on this hub.  Returns 0 on success, -1
 * for a NULL hub or select set, an `ev` outside [0, POLL_EV_MAX), or an
 * allocation failure.
 *
 * `sel` is BORROWED: the node stores the bare pointer, so the select set must be
 * unregistered with poll_struct_remove before it is freed, or poll_notify will
 * signal freed memory.  ipc_select_destroy is the path that guarantees this.
 * Duplicate registrations are NOT collapsed — adding the same `sel` twice leaves
 * two nodes and signals it twice — so the caller de-duplicates (ipc_select_add
 * does, by scanning its own ep_ids first).  user_data is recorded on the node
 * but nothing in this file reads it back; the signal carries the endpoint id. */
int poll_struct_add(poll_struct_t* ps, poll_ev_t ev, struct ipc_select* sel, uint32_t user_data) {
    if (!ps || (int)ev < 0 || ev >= POLL_EV_MAX || !sel) {
        return -1;
    }
    poll_watcher_t* w = (poll_watcher_t*)malloc(sizeof(poll_watcher_t));
    if (!w) {
        return -1;
    }
    w->sel = sel;
    w->user_data = user_data;
    w->next = ps->watchers[ev];
    ps->watchers[ev] = w;
    return 0;
}

/* Unregisters EVERY node naming `sel`, across all event kinds, and frees them.
 * Removing a set that was never registered is a no-op, so it is safe to call
 * once per endpoint a select set believes it watches without checking first. */
void poll_struct_remove(poll_struct_t* ps, struct ipc_select* sel) {
    if (!ps || !sel) {
        return;
    }
    for (int ev = 0; ev < POLL_EV_MAX; ev++) {
        poll_watcher_t** pp = &ps->watchers[ev];
        while (*pp) {
            poll_watcher_t* w = *pp;
            if (w->sel == sel) {
                *pp = w->next;
                free(w);
            } else {
                pp = &w->next;
            }
        }
    }
}

/* Pushes readiness for `ep_id` to every set watching `ev`, in registration
 * order.  Cannot fail and reports nothing: a hub with no watchers, a NULL hub
 * and an out-of-range `ev` are all silent no-ops, because a send must not be
 * penalised for the absence of watchers.  Each signal is a latch write plus at
 * most one thread wake (ipc_select_signal), so this runs in O(watchers) with the
 * calling endpoint's ep->lock held for the whole walk. */
void poll_notify(poll_struct_t* ps, poll_ev_t ev, uint32_t ep_id) {
    if (!ps || (int)ev < 0 || ev >= POLL_EV_MAX) {
        return;
    }
    poll_watcher_t* w = ps->watchers[ev];
    while (w) {
        ipc_select_signal(w->sel, ep_id);
        w = w->next;
    }
}

/* Frees the hub and every watcher node on it.  The watched select sets are NOT
 * notified and NOT touched — they are borrowed, and a set outliving its endpoint
 * simply stops being signalled by it (ipc_endpoint_teardown aborts anyone parked
 * on the endpoint separately).  The hub pointer is invalid on return; the caller
 * clears ep->poll_struct. */
void poll_struct_free(poll_struct_t* ps) {
    if (!ps) {
        return;
    }
    for (int ev = 0; ev < POLL_EV_MAX; ev++) {
        poll_watcher_t* w = ps->watchers[ev];
        while (w) {
            poll_watcher_t* next = w->next;
            free(w);
            w = next;
        }
    }
#ifdef WASMOS_POLL_TEST_SEAMS
    g_poll_live_structs--;
#endif
    free(ps);
}
