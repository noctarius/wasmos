#ifdef WASMOS_SCHED_THREADABLE

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
 */

poll_struct_t *
poll_struct_alloc(void)
{
    poll_struct_t *ps = (poll_struct_t *)malloc(sizeof(poll_struct_t));
    if (!ps) {
        return 0;
    }
    memset(ps, 0, sizeof(*ps));
    return ps;
}

int
poll_struct_add(poll_struct_t *ps, poll_ev_t ev,
                struct ipc_select *sel, uint32_t user_data)
{
    if (!ps || (int)ev < 0 || ev >= POLL_EV_MAX || !sel) {
        return -1;
    }
    poll_watcher_t *w = (poll_watcher_t *)malloc(sizeof(poll_watcher_t));
    if (!w) {
        return -1;
    }
    w->sel       = sel;
    w->user_data = user_data;
    w->next      = ps->watchers[ev];
    ps->watchers[ev] = w;
    return 0;
}

void
poll_struct_remove(poll_struct_t *ps, struct ipc_select *sel)
{
    if (!ps || !sel) {
        return;
    }
    for (int ev = 0; ev < POLL_EV_MAX; ev++) {
        poll_watcher_t **pp = &ps->watchers[ev];
        while (*pp) {
            poll_watcher_t *w = *pp;
            if (w->sel == sel) {
                *pp = w->next;
                free(w);
            } else {
                pp = &w->next;
            }
        }
    }
}

void
poll_notify(poll_struct_t *ps, poll_ev_t ev, uint32_t ep_id)
{
    if (!ps || (int)ev < 0 || ev >= POLL_EV_MAX) {
        return;
    }
    poll_watcher_t *w = ps->watchers[ev];
    while (w) {
        ipc_select_signal(w->sel, ep_id);
        w = w->next;
    }
}

void
poll_struct_free(poll_struct_t *ps)
{
    if (!ps) {
        return;
    }
    for (int ev = 0; ev < POLL_EV_MAX; ev++) {
        poll_watcher_t *w = ps->watchers[ev];
        while (w) {
            poll_watcher_t *next = w->next;
            free(w);
            w = next;
        }
    }
    free(ps);
}

#endif /* WASMOS_SCHED_THREADABLE */
