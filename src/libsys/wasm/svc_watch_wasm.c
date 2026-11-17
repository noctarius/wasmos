/* svc_watch_wasm.c - service-name watches and awaits over the libsys event loop.
 * Contract in wasmos/libsys.h ("service-name watches"). */
#include "wasmos/libsys.h"

/* The packed SVC_IPC_LOOKUP_REQ carries at most this many name bytes. */
#define SVC_WATCH_LOOKUP_NAME_MAX 16u

static uint32_t svc_watch_now(void) {
    return (uint32_t)wasmos_sched_ticks();
}

/* Rounds UP, as the kernel's timer_ms_to_ticks does, so a positive window never
 * becomes a deadline that has already passed. */
static uint32_t svc_watch_ms_to_ticks(int32_t ms) {
    return (uint32_t)(((uint64_t)(uint32_t)ms * WASMOS_SCHED_TICK_HZ + 999u) / 1000u);
}

/* The status a non-RESP reply carries: the packed code in arg1 of an
 * SVC_IPC_ERROR, else SVC_WATCH_BAD_REPLY. */
static int32_t svc_watch_reply_status(const wasmos_ipc_message_t* msg) {
    if (msg->type == SVC_IPC_ERROR && msg->arg1 < 0) {
        return msg->arg1;
    }
    return WASMOS_ERR_PROC_PM_SVC_WATCH_BAD_REPLY;
}

static void svc_watch_unlink(wasmos_sys_svc_watch_t* watch) {
    wasmos_sys_svc_watch_t** link = &watch->set->head;
    while (*link) {
        if (*link == watch) {
            *link = watch->next;
            break;
        }
        link = &(*link)->next;
    }
    watch->next = 0;
}

static void svc_watch_release_buffer(wasmos_sys_svc_watch_t* watch) {
    if (watch->buffer_id >= 0) {
        (void)wasmos_xfer_buffer_release(watch->buffer_id);
        watch->buffer_id = -1;
    }
}

/* Consumes the reply to an UNSUBSCRIBE_NAME, which needs no action. */
static void svc_watch_on_unsubscribed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

void wasmos_sys_svc_watch_stop(wasmos_sys_svc_watch_t* watch) {
    wasmos_sys_event_loop_t* loop;
    if (!watch || !watch->active) {
        return;
    }
    loop = watch->set->loop;
    watch->active = 0;
    svc_watch_unlink(watch);
    if (watch->subscribe_request > 0) {
        wasmos_sys_intent_cancel(loop, watch->subscribe_request);
        watch->subscribe_request = 0;
    }
    if (watch->lookup_request > 0) {
        wasmos_sys_intent_cancel(loop, watch->lookup_request);
        watch->lookup_request = 0;
    }
    svc_watch_release_buffer(watch);
    /* Sent even when the subscribe reply has not arrived: the process manager
     * serves one sender's requests in order, so this lands after the subscribe
     * and removes what it created. Unsubscribing a key that does not exist is
     * not an error. A failed send leaves the subscription until this process
     * exits; its events then match no watch and are dropped. */
    (void)wasmos_sys_intent_send(loop,
                                 watch->set->proc_endpoint,
                                 loop->receiver_endpoint,
                                 SVC_IPC_UNSUBSCRIBE_NAME_REQ,
                                 loop->receiver_endpoint,
                                 (int32_t)watch->cookie,
                                 0,
                                 0,
                                 svc_watch_on_unsubscribed,
                                 0,
                                 0);
}

static void svc_watch_fail(wasmos_sys_svc_watch_t* watch, int32_t status) {
    wasmos_sys_svc_fail_fn on_fail = watch->on_fail;
    void* user = watch->user;
    wasmos_sys_svc_watch_stop(watch);
    if (on_fail) {
        on_fail(user, watch, status);
    }
}

static void svc_watch_bind(wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    if (endpoint < 0 || endpoint == watch->endpoint) {
        return;
    }
    watch->endpoint = endpoint;
    watch->has_deadline = 0;
    if (watch->on_bind) {
        watch->on_bind(watch->user, watch, endpoint);
    }
}

static void svc_watch_on_lookup(void* user, const wasmos_ipc_message_t* msg) {
    wasmos_sys_svc_watch_t* watch = (wasmos_sys_svc_watch_t*)user;
    watch->lookup_request = 0;
    if (msg->type != SVC_IPC_LOOKUP_RESP) {
        svc_watch_fail(watch, svc_watch_reply_status(msg));
        return;
    }
    /* An unregistered name is a successful lookup carrying 0xFFFFFFFF; the
     * subscription then reports the first registration. */
    if ((uint32_t)msg->arg0 != 0xFFFFFFFFu) {
        svc_watch_bind(watch, msg->arg0);
    }
}

static void svc_watch_on_subscribed(void* user, const wasmos_ipc_message_t* msg) {
    wasmos_sys_svc_watch_t* watch = (wasmos_sys_svc_watch_t*)user;
    wasmos_sys_event_loop_t* loop = watch->set->loop;
    int32_t args[4];
    int32_t request_id = 0;

    watch->subscribe_request = 0;
    svc_watch_release_buffer(watch);
    if (msg->type != SVC_IPC_SUBSCRIBE_NAME_RESP) {
        svc_watch_fail(watch, svc_watch_reply_status(msg));
        return;
    }
    wasmos_ipc_pack_name16(watch->name, args);
    if (wasmos_sys_intent_send(loop,
                               watch->set->proc_endpoint,
                               loop->receiver_endpoint,
                               SVC_IPC_LOOKUP_REQ,
                               args[0],
                               args[1],
                               args[2],
                               args[3],
                               svc_watch_on_lookup,
                               watch,
                               &request_id) != 0) {
        svc_watch_fail(watch, WASMOS_ERR_PROC_PM_SVC_WATCH_SEND);
        return;
    }
    watch->lookup_request = request_id;
}

/* SVC_IPC_NAME_EVENT handler: routes the event to the watch whose cookie it
 * echoes. An event for no active watch belongs to one that was stopped. */
static void svc_watch_on_event(void* user, const wasmos_ipc_message_t* msg) {
    wasmos_sys_svc_watch_set_t* set = (wasmos_sys_svc_watch_set_t*)user;
    if (msg->arg0 != SVC_NAME_EVENT_BIND) {
        return;
    }
    for (wasmos_sys_svc_watch_t* w = set->head; w; w = w->next) {
        if (w->cookie == (uint32_t)msg->arg1) {
            svc_watch_bind(w, msg->arg2);
            return;
        }
    }
}

int32_t wasmos_sys_svc_watch_set_init(wasmos_sys_svc_watch_set_t* set,
                                      wasmos_sys_event_loop_t* loop, int32_t proc_endpoint) {
    if (!set || !loop) {
        return -1;
    }
    set->loop = loop;
    set->proc_endpoint = proc_endpoint;
    set->next_cookie = 1;
    set->head = 0;
    return wasmos_sys_event_register(loop, SVC_IPC_NAME_EVENT, svc_watch_on_event, set);
}

int32_t wasmos_sys_svc_watch_start(wasmos_sys_svc_watch_set_t* set, wasmos_sys_svc_watch_t* watch,
                                   const char* name, int32_t timeout_ms,
                                   wasmos_sys_svc_bind_fn on_bind, wasmos_sys_svc_fail_fn on_fail,
                                   void* user) {
    svc_name_subscribe_desc_t desc;
    wasmos_sys_event_loop_t* loop;
    int32_t request_id = 0;
    uint32_t len;

    if (!set || !set->loop || !watch || watch->active || !name || timeout_ms < 0) {
        return -1;
    }
    len = (uint32_t)strlen(name);
    if (len == 0 || len > SVC_WATCH_LOOKUP_NAME_MAX) {
        return -1;
    }
    loop = set->loop;
    memset(watch, 0, sizeof(*watch));
    watch->set = set;
    watch->on_bind = on_bind;
    watch->on_fail = on_fail;
    watch->user = user;
    watch->endpoint = -1;
    watch->buffer_id = -1;
    memcpy(watch->name, name, len + 1u);
    if (set->next_cookie == 0) {
        set->next_cookie = 1;
    }
    watch->cookie = set->next_cookie++;
    if (timeout_ms > 0) {
        watch->deadline_tick = svc_watch_now() + svc_watch_ms_to_ticks(timeout_ms);
        watch->has_deadline = 1;
    }

    memset(&desc, 0, sizeof(desc));
    desc.version = WASMOS_SVC_NAME_SUBSCRIBE_DESC_VERSION;
    desc.notify_endpoint = (uint32_t)loop->receiver_endpoint;
    desc.cookie = watch->cookie;
    memcpy(desc.name, name, len + 1u);
    watch->buffer_id = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (watch->buffer_id < 0) {
        watch->buffer_id = -1;
        return WASMOS_ERR_PROC_PM_SVC_WATCH_NO_BUFFER;
    }
    if (wasmos_sys_intent_send(loop,
                               set->proc_endpoint,
                               loop->receiver_endpoint,
                               SVC_IPC_SUBSCRIBE_NAME_REQ,
                               watch->buffer_id,
                               0,
                               (int32_t)sizeof(desc),
                               0,
                               svc_watch_on_subscribed,
                               watch,
                               &request_id) != 0) {
        svc_watch_release_buffer(watch);
        return WASMOS_ERR_PROC_PM_SVC_WATCH_SEND;
    }
    watch->subscribe_request = request_id;
    watch->active = 1;
    watch->next = set->head;
    set->head = watch;
    return 0;
}

/* Restarts the walk after every failure, because failing a watch unlinks it and
 * its callback may start or stop others. */
void wasmos_sys_svc_watch_set_expire(wasmos_sys_svc_watch_set_t* set) {
    uint32_t now;
    int expired;
    if (!set) {
        return;
    }
    now = svc_watch_now();
    do {
        expired = 0;
        for (wasmos_sys_svc_watch_t* w = set->head; w; w = w->next) {
            /* Wrap-safe: the tick counter is 32 bits. */
            if (w->has_deadline && (int32_t)(now - w->deadline_tick) >= 0) {
                svc_watch_fail(w, WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT);
                expired = 1;
                break;
            }
        }
    } while (expired);
}

static void svc_await_on_bind(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    wasmos_sys_svc_await_t* await = (wasmos_sys_svc_await_t*)user;
    wasmos_sys_svc_watch_stop(watch);
    (void)wasmos_promise_resolve(&await->promise, (uintptr_t)(uint32_t)endpoint);
}

static void svc_await_on_fail(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    wasmos_sys_svc_await_t* await = (wasmos_sys_svc_await_t*)user;
    (void)watch;
    (void)wasmos_promise_reject(&await->promise, status);
}

wasmos_future_t* wasmos_sys_svc_await_start(wasmos_sys_svc_watch_set_t* set,
                                            wasmos_sys_svc_await_t* await, const char* name,
                                            int32_t timeout_ms) {
    int32_t rc;
    if (!set || !await || await->watch.active) {
        return 0;
    }
    wasmos_future_init(&await->future, &await->promise);
    rc = wasmos_sys_svc_watch_start(
        set, &await->watch, name, timeout_ms, svc_await_on_bind, svc_await_on_fail, await);
    if (rc == -1) {
        return 0;
    }
    if (rc != 0) {
        (void)wasmos_promise_reject(&await->promise, rc);
    }
    return &await->future;
}
