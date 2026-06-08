#ifndef WASMOS_POLL_H
#define WASMOS_POLL_H

#ifdef WASMOS_SCHED_THREADABLE

#include <stdint.h>

#define POLL_EV_MAX 4

typedef enum {
    POLL_EV_IN     = 0,  /* data / message ready to read */
    POLL_EV_OUT    = 1,  /* space available to send */
    POLL_EV_CLOSE  = 2,  /* far end closed / endpoint destroyed */
    POLL_EV_KERNEL = 3,  /* kernel-internal (timer, IRQ, etc.) */
} poll_ev_t;

struct ipc_select;

typedef struct poll_watcher {
    struct ipc_select  *sel;
    uint32_t            user_data;
    struct poll_watcher *next;
} poll_watcher_t;

typedef struct {
    poll_watcher_t *watchers[POLL_EV_MAX];
} poll_struct_t;

/* Allocate and zero-initialise a poll_struct_t from the kernel heap. */
poll_struct_t *poll_struct_alloc(void);

/* Register sel as a watcher for ev on ps. */
int poll_struct_add(poll_struct_t *ps, poll_ev_t ev,
                    struct ipc_select *sel, uint32_t user_data);

/* Remove all watcher entries for sel from ps (all event types). */
void poll_struct_remove(poll_struct_t *ps, struct ipc_select *sel);

/*
 * Notify all watchers registered for ev on ps about endpoint ep_id.
 * Safe to call with ps == NULL (no-op).
 */
void poll_notify(poll_struct_t *ps, poll_ev_t ev, uint32_t ep_id);

/* Free ps and all embedded watcher nodes. */
void poll_struct_free(poll_struct_t *ps);

#endif /* WASMOS_SCHED_THREADABLE */
#endif /* WASMOS_POLL_H */
