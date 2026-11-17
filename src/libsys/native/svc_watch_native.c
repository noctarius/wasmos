/* svc_watch_native.c - native service-name watches and awaits. The contract is
 * the WASM one in wasmos/libsys.h ("service-name watches"); this file differs
 * only in transport: a native event loop, a mapped transfer buffer, and
 * api->sched_ticks for the clock. */
#include "wasmos/libsys_native.h"

/* The packed SVC_IPC_LOOKUP_REQ carries at most this many name bytes. */
#define SVC_WATCH_LOOKUP_NAME_MAX 16u

static uint32_t native_svc_watch_strlen(const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void native_svc_watch_zero(void* p, uint32_t len) {
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < len; ++i) {
        b[i] = 0;
    }
}

static uint32_t native_svc_watch_now(const wasmos_sys_native_svc_watch_set_t* set) {
    return set->loop->api->sched_ticks ? set->loop->api->sched_ticks() : 0u;
}

/* Rounds UP, so a positive window never becomes a deadline already passed. */
static uint32_t native_svc_watch_ms_to_ticks(int32_t ms) {
    return (uint32_t)(((uint64_t)(uint32_t)ms * WASMOS_SCHED_TICK_HZ + 999u) / 1000u);
}

static int32_t native_svc_watch_reply_status(const nd_ipc_message_t* msg) {
    if (msg->type == SVC_IPC_ERROR && (int32_t)msg->arg1 < 0) {
        return (int32_t)msg->arg1;
    }
    return WASMOS_ERR_PROC_PM_SVC_WATCH_BAD_REPLY;
}

static void native_svc_watch_unlink(wasmos_sys_native_svc_watch_t* watch) {
    wasmos_sys_native_svc_watch_t** link = &watch->set->head;
    while (*link) {
        if (*link == watch) {
            *link = watch->next;
            break;
        }
        link = &(*link)->next;
    }
    watch->next = 0;
}

static void native_svc_watch_release_buffer(wasmos_sys_native_svc_watch_t* watch) {
    if (watch->has_buffer) {
        (void)watch->set->loop->api->xfer_buffer_release(watch->buffer_id);
        watch->has_buffer = 0;
    }
}

static void native_svc_watch_on_unsubscribed(void* user, const nd_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

void wasmos_sys_native_svc_watch_stop(wasmos_sys_native_svc_watch_t* watch) {
    wasmos_sys_native_event_loop_t* loop;
    if (!watch || !watch->active) {
        return;
    }
    loop = watch->set->loop;
    watch->active = 0;
    native_svc_watch_unlink(watch);
    if (watch->subscribe_request != 0u) {
        wasmos_sys_native_intent_cancel(loop, watch->subscribe_request);
        watch->subscribe_request = 0u;
    }
    if (watch->lookup_request != 0u) {
        wasmos_sys_native_intent_cancel(loop, watch->lookup_request);
        watch->lookup_request = 0u;
    }
    native_svc_watch_release_buffer(watch);
    /* Ordered after the subscribe by the process manager, so it removes what the
     * subscribe created even when the reply has not arrived; see the WASM
     * variant. */
    (void)wasmos_sys_native_intent_send(loop,
                                        watch->set->proc_endpoint,
                                        loop->receiver_endpoint,
                                        SVC_IPC_UNSUBSCRIBE_NAME_REQ,
                                        loop->receiver_endpoint,
                                        watch->cookie,
                                        0u,
                                        0u,
                                        native_svc_watch_on_unsubscribed,
                                        0,
                                        0);
}

static void native_svc_watch_fail(wasmos_sys_native_svc_watch_t* watch, int32_t status) {
    wasmos_sys_native_svc_fail_fn on_fail = watch->on_fail;
    void* user = watch->user;
    wasmos_sys_native_svc_watch_stop(watch);
    if (on_fail) {
        on_fail(user, watch, status);
    }
}

static void native_svc_watch_bind(wasmos_sys_native_svc_watch_t* watch, uint32_t endpoint) {
    if (watch->bound && endpoint == watch->endpoint) {
        return;
    }
    watch->endpoint = endpoint;
    watch->bound = 1;
    watch->has_deadline = 0;
    if (watch->on_bind) {
        watch->on_bind(watch->user, watch, endpoint);
    }
}

static void native_svc_watch_on_lookup(void* user, const nd_ipc_message_t* msg) {
    wasmos_sys_native_svc_watch_t* watch = (wasmos_sys_native_svc_watch_t*)user;
    watch->lookup_request = 0u;
    if (msg->type != SVC_IPC_LOOKUP_RESP) {
        native_svc_watch_fail(watch, native_svc_watch_reply_status(msg));
        return;
    }
    if (msg->arg0 != 0xFFFFFFFFu) {
        native_svc_watch_bind(watch, msg->arg0);
    }
}

static void native_svc_watch_on_subscribed(void* user, const nd_ipc_message_t* msg) {
    wasmos_sys_native_svc_watch_t* watch = (wasmos_sys_native_svc_watch_t*)user;
    wasmos_sys_native_event_loop_t* loop = watch->set->loop;
    uint32_t args[4];
    uint32_t request_id = 0u;

    watch->subscribe_request = 0u;
    native_svc_watch_release_buffer(watch);
    if (msg->type != SVC_IPC_SUBSCRIBE_NAME_RESP) {
        native_svc_watch_fail(watch, native_svc_watch_reply_status(msg));
        return;
    }
    wasmos_sys_ipc_pack_name16_native(
        (const uint8_t*)watch->name, native_svc_watch_strlen(watch->name), args);
    if (wasmos_sys_native_intent_send(loop,
                                      watch->set->proc_endpoint,
                                      loop->receiver_endpoint,
                                      SVC_IPC_LOOKUP_REQ,
                                      args[0],
                                      args[1],
                                      args[2],
                                      args[3],
                                      native_svc_watch_on_lookup,
                                      watch,
                                      &request_id) != 0) {
        native_svc_watch_fail(watch, WASMOS_ERR_PROC_PM_SVC_WATCH_SEND);
        return;
    }
    watch->lookup_request = request_id;
}

static void native_svc_watch_on_event(void* user, const nd_ipc_message_t* msg) {
    wasmos_sys_native_svc_watch_set_t* set = (wasmos_sys_native_svc_watch_set_t*)user;
    if (msg->arg0 != SVC_NAME_EVENT_BIND) {
        return;
    }
    for (wasmos_sys_native_svc_watch_t* w = set->head; w; w = w->next) {
        if (w->cookie == msg->arg1) {
            native_svc_watch_bind(w, msg->arg2);
            return;
        }
    }
}

int32_t wasmos_sys_native_svc_watch_set_init(wasmos_sys_native_svc_watch_set_t* set,
                                             wasmos_sys_native_event_loop_t* loop,
                                             uint32_t proc_endpoint) {
    if (!set || !loop) {
        return -1;
    }
    set->loop = loop;
    set->proc_endpoint = proc_endpoint;
    set->next_cookie = 1u;
    set->head = 0;
    return wasmos_sys_native_event_register(
        loop, SVC_IPC_NAME_EVENT, native_svc_watch_on_event, set);
}

int32_t wasmos_sys_native_svc_watch_start(wasmos_sys_native_svc_watch_set_t* set,
                                          wasmos_sys_native_svc_watch_t* watch, const char* name,
                                          int32_t timeout_ms, wasmos_sys_native_svc_bind_fn on_bind,
                                          wasmos_sys_native_svc_fail_fn on_fail, void* user) {
    wasmos_sys_native_event_loop_t* loop;
    svc_name_subscribe_desc_t* desc;
    uint32_t request_id = 0u;
    uint32_t len;

    if (!set || !set->loop || !set->loop->api || !set->loop->api->xfer_buffer_acquire ||
        !set->loop->api->xfer_buffer_release || !watch || watch->active || !name ||
        timeout_ms < 0) {
        return -1;
    }
    len = native_svc_watch_strlen(name);
    if (len == 0u || len > SVC_WATCH_LOOKUP_NAME_MAX) {
        return -1;
    }
    loop = set->loop;
    native_svc_watch_zero(watch, (uint32_t)sizeof(*watch));
    watch->set = set;
    watch->on_bind = on_bind;
    watch->on_fail = on_fail;
    watch->user = user;
    for (uint32_t i = 0; i <= len; ++i) {
        watch->name[i] = name[i];
    }
    if (set->next_cookie == 0u) {
        set->next_cookie = 1u;
    }
    watch->cookie = set->next_cookie++;
    if (timeout_ms > 0) {
        watch->deadline_tick = native_svc_watch_now(set) + native_svc_watch_ms_to_ticks(timeout_ms);
        watch->has_deadline = 1;
    }

    desc = (svc_name_subscribe_desc_t*)loop->api->xfer_buffer_acquire(
        ND_BUFFER_KIND_XFER, (uint32_t)sizeof(*desc), &watch->buffer_id);
    if (!desc) {
        return WASMOS_ERR_PROC_PM_SVC_WATCH_NO_BUFFER;
    }
    watch->has_buffer = 1;
    native_svc_watch_zero(desc, (uint32_t)sizeof(*desc));
    desc->version = WASMOS_SVC_NAME_SUBSCRIBE_DESC_VERSION;
    desc->notify_endpoint = loop->receiver_endpoint;
    desc->cookie = watch->cookie;
    for (uint32_t i = 0; i <= len; ++i) {
        desc->name[i] = name[i];
    }
    if (wasmos_sys_native_intent_send(loop,
                                      set->proc_endpoint,
                                      loop->receiver_endpoint,
                                      SVC_IPC_SUBSCRIBE_NAME_REQ,
                                      watch->buffer_id,
                                      0u,
                                      (uint32_t)sizeof(*desc),
                                      0u,
                                      native_svc_watch_on_subscribed,
                                      watch,
                                      &request_id) != 0) {
        native_svc_watch_release_buffer(watch);
        return WASMOS_ERR_PROC_PM_SVC_WATCH_SEND;
    }
    watch->subscribe_request = request_id;
    watch->active = 1;
    watch->next = set->head;
    set->head = watch;
    return 0;
}

void wasmos_sys_native_svc_watch_set_expire(wasmos_sys_native_svc_watch_set_t* set) {
    uint32_t now;
    int expired;
    if (!set || !set->loop || !set->loop->api) {
        return;
    }
    now = native_svc_watch_now(set);
    do {
        expired = 0;
        for (wasmos_sys_native_svc_watch_t* w = set->head; w; w = w->next) {
            if (w->has_deadline && (int32_t)(now - w->deadline_tick) >= 0) {
                native_svc_watch_fail(w, WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT);
                expired = 1;
                break;
            }
        }
    } while (expired);
}

static void native_svc_await_on_bind(void* user, wasmos_sys_native_svc_watch_t* watch,
                                     uint32_t endpoint) {
    wasmos_sys_native_svc_await_t* await = (wasmos_sys_native_svc_await_t*)user;
    wasmos_sys_native_svc_watch_stop(watch);
    (void)wasmos_promise_resolve(&await->promise, (uintptr_t)endpoint);
}

static void native_svc_await_on_fail(void* user, wasmos_sys_native_svc_watch_t* watch,
                                     int32_t status) {
    wasmos_sys_native_svc_await_t* await = (wasmos_sys_native_svc_await_t*)user;
    (void)watch;
    (void)wasmos_promise_reject(&await->promise, status);
}

wasmos_future_t* wasmos_sys_native_svc_await_start(wasmos_sys_native_svc_watch_set_t* set,
                                                   wasmos_sys_native_svc_await_t* await,
                                                   const char* name, int32_t timeout_ms) {
    int32_t rc;
    if (!set || !await || await->watch.active) {
        return 0;
    }
    wasmos_future_init(&await->future, &await->promise);
    rc = wasmos_sys_native_svc_watch_start(set,
                                           &await->watch,
                                           name,
                                           timeout_ms,
                                           native_svc_await_on_bind,
                                           native_svc_await_on_fail,
                                           await);
    if (rc == -1) {
        return 0;
    }
    if (rc != 0) {
        (void)wasmos_promise_reject(&await->promise, rc);
    }
    return &await->future;
}
