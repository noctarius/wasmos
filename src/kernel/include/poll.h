#ifndef WASMOS_POLL_H
#define WASMOS_POLL_H

#include <stdint.h>

/* Event kinds a hub can carry, and the size of its per-kind watcher array.
 * Only POLL_EV_IN is raised today: ipc_send_from and ipc_notify_from push it,
 * and nothing signals the other three. */
#define POLL_EV_MAX 4

typedef enum {
    POLL_EV_IN = 0,     /* data / message ready to read */
    POLL_EV_OUT = 1,    /* space available to send */
    POLL_EV_CLOSE = 2,  /* far end closed / endpoint destroyed */
    POLL_EV_KERNEL = 3, /* kernel-internal (timer, IRQ, etc.) */
} poll_ev_t;

struct ipc_select;

/* One registration. `sel` is borrowed: the hub never frees the select set, and
 * the set must be unregistered (poll_struct_remove) before it is destroyed,
 * otherwise a later notify signals freed memory. `user_data` is stored but not
 * currently passed on by poll_notify. */
typedef struct poll_watcher {
    struct ipc_select* sel;
    uint32_t user_data;
    struct poll_watcher* next;
} poll_watcher_t;

/* The hub an endpoint owns. Allocated lazily on the first watcher and reachable
 * only from that endpoint, which frees it at teardown. Carries no lock: every
 * operation below runs under the owning endpoint's lock. */
typedef struct {
    poll_watcher_t* watchers[POLL_EV_MAX];
} poll_struct_t;

/* Allocate and zero-initialise a poll_struct_t from the kernel heap.
 * Returns NULL when the allocation fails; the caller owns the result and must
 * eventually pass it to poll_struct_free. */
poll_struct_t* poll_struct_alloc(void);

/* Register sel as a watcher for ev on ps.
 * Returns 0, or -1 for a NULL ps or sel, an out-of-range ev, or a failed
 * allocation. Does NOT deduplicate: registering the same set twice leaves two
 * watchers and signals it twice per event, so the caller must keep its own
 * "already watching" check (ipc_select_add does). */
int poll_struct_add(poll_struct_t* ps, poll_ev_t ev, struct ipc_select* sel, uint32_t user_data);

/* Remove all watcher entries for sel from ps (all event types).
 * Frees the watcher nodes; the select set itself is untouched. A set that was
 * never registered is a silent no-op. */
void poll_struct_remove(poll_struct_t* ps, struct ipc_select* sel);

/*
 * Notify all watchers registered for ev on ps about endpoint ep_id.
 * Safe to call with ps == NULL (no-op).
 * O(watchers), not O(endpoints): each registered set is signalled directly.
 * Never blocks, but it does take each signalled set's event lock, so the caller
 * must respect the IPC lock order -- endpoint lock held, no event lock held.
 */
void poll_notify(poll_struct_t* ps, poll_ev_t ev, uint32_t ep_id);

/* Free ps and all embedded watcher nodes.  The watched select sets are not
 * touched; any that is still registered is simply forgotten. NULL is a no-op. */
void poll_struct_free(poll_struct_t* ps);

#ifdef WASMOS_POLL_TEST_SEAMS
/* Outstanding poll hubs, so a test can observe a release that is otherwise
 * invisible: the owning endpoint holds the only pointer and drops it. */
uint32_t poll_test_live_structs(void);
#endif

#endif /* WASMOS_POLL_H */
