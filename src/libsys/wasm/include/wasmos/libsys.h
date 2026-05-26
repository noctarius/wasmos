#ifndef WASMOS_LIBSYS_H
#define WASMOS_LIBSYS_H

#include <stdint.h>

#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/sha256.h"
#include "wasmos/libsys_string.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASMOS_SYS_INTENT_MAX 16
#define WASMOS_SYS_HANDLER_MAX 16

typedef struct {
    int32_t in_use;
    int32_t request_id;
    void (*on_resolve)(void *user, const wasmos_ipc_message_t *msg);
    void *user;
} wasmos_sys_intent_t;

typedef struct {
    int32_t in_use;
    int32_t msg_type;
    void (*on_message)(void *user, const wasmos_ipc_message_t *msg);
    void *user;
} wasmos_sys_handler_t;

typedef struct {
    int32_t receiver_endpoint;
    int32_t next_request_id;
    void (*default_on_message)(void *user, const wasmos_ipc_message_t *msg);
    void *default_user;
    wasmos_sys_intent_t intents[WASMOS_SYS_INTENT_MAX];
    wasmos_sys_handler_t handlers[WASMOS_SYS_HANDLER_MAX];
} wasmos_sys_event_loop_t;

static inline int32_t
wasmos_sys_ipc_recv_matching(int32_t reply_endpoint,
                             int32_t request_id,
                             wasmos_ipc_message_t *out_reply)
{
    for (;;) {
        if (wasmos_ipc_recv(reply_endpoint) < 0) {
            return -1;
        }
        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);
        if (msg.request_id != request_id) {
            continue;
        }
        if (out_reply) {
            *out_reply = msg;
        }
        return 0;
    }
}

static inline void
wasmos_sys_event_loop_init(wasmos_sys_event_loop_t *loop,
                           int32_t receiver_endpoint,
                           int32_t request_id_base)
{
    if (!loop) {
        return;
    }
    loop->receiver_endpoint = receiver_endpoint;
    loop->next_request_id = request_id_base;
    loop->default_on_message = 0;
    loop->default_user = 0;
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        loop->intents[i].in_use = 0;
        loop->intents[i].request_id = 0;
        loop->intents[i].on_resolve = 0;
        loop->intents[i].user = 0;
    }
    for (int32_t i = 0; i < WASMOS_SYS_HANDLER_MAX; ++i) {
        loop->handlers[i].in_use = 0;
        loop->handlers[i].msg_type = 0;
        loop->handlers[i].on_message = 0;
        loop->handlers[i].user = 0;
    }
}

static inline int32_t
wasmos_sys_event_set_default(wasmos_sys_event_loop_t *loop,
                             void (*on_message)(void *user, const wasmos_ipc_message_t *msg),
                             void *user)
{
    if (!loop || !on_message) {
        return -1;
    }
    loop->default_on_message = on_message;
    loop->default_user = user;
    return 0;
}

static inline int32_t
wasmos_sys_event_register(wasmos_sys_event_loop_t *loop,
                          int32_t msg_type,
                          void (*on_message)(void *user, const wasmos_ipc_message_t *msg),
                          void *user)
{
    if (!loop || !on_message) {
        return -1;
    }
    for (int32_t i = 0; i < WASMOS_SYS_HANDLER_MAX; ++i) {
        if (loop->handlers[i].in_use && loop->handlers[i].msg_type == msg_type) {
            loop->handlers[i].on_message = on_message;
            loop->handlers[i].user = user;
            return 0;
        }
    }
    for (int32_t i = 0; i < WASMOS_SYS_HANDLER_MAX; ++i) {
        if (!loop->handlers[i].in_use) {
            loop->handlers[i].in_use = 1;
            loop->handlers[i].msg_type = msg_type;
            loop->handlers[i].on_message = on_message;
            loop->handlers[i].user = user;
            return 0;
        }
    }
    return -1;
}

static inline int32_t
wasmos_sys_intent_send(wasmos_sys_event_loop_t *loop,
                       int32_t destination_endpoint,
                       int32_t source_endpoint,
                       int32_t type,
                       int32_t arg0,
                       int32_t arg1,
                       int32_t arg2,
                       int32_t arg3,
                       void (*on_resolve)(void *user, const wasmos_ipc_message_t *msg),
                       void *user,
                       int32_t *out_request_id)
{
    int32_t request_id = 0;
    if (!loop || !on_resolve) {
        return -1;
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (!loop->intents[i].in_use) {
            request_id = loop->next_request_id++;
            if (wasmos_ipc_send(destination_endpoint,
                                source_endpoint,
                                type,
                                request_id,
                                arg0,
                                arg1,
                                arg2,
                                arg3) != 0) {
                return -1;
            }
            loop->intents[i].in_use = 1;
            loop->intents[i].request_id = request_id;
            loop->intents[i].on_resolve = on_resolve;
            loop->intents[i].user = user;
            if (out_request_id) {
                *out_request_id = request_id;
            }
            return 0;
        }
    }
    return -1;
}

static inline int32_t
wasmos_sys_intent_send_with_request_id(wasmos_sys_event_loop_t *loop,
                                       int32_t destination_endpoint,
                                       int32_t source_endpoint,
                                       int32_t request_id,
                                       int32_t type,
                                       int32_t arg0,
                                       int32_t arg1,
                                       int32_t arg2,
                                       int32_t arg3,
                                       void (*on_resolve)(void *user, const wasmos_ipc_message_t *msg),
                                       void *user)
{
    if (!loop || !on_resolve || request_id <= 0) {
        return -1;
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (loop->intents[i].in_use && loop->intents[i].request_id == request_id) {
            return -1;
        }
    }
    for (int32_t i = 0; i < WASMOS_SYS_INTENT_MAX; ++i) {
        if (!loop->intents[i].in_use) {
            if (wasmos_ipc_send(destination_endpoint,
                                source_endpoint,
                                type,
                                request_id,
                                arg0,
                                arg1,
                                arg2,
                                arg3) != 0) {
                return -1;
            }
            loop->intents[i].in_use = 1;
            loop->intents[i].request_id = request_id;
            loop->intents[i].on_resolve = on_resolve;
            loop->intents[i].user = user;
            return 0;
        }
    }
    return -1;
}

static inline int32_t
wasmos_sys_event_loop_poll(wasmos_sys_event_loop_t *loop, int32_t budget)
{
    int32_t handled = 0;
    if (!loop) {
        return 0;
    }
    if (budget == 0) {
        budget = 1;
    }
    for (int32_t i = 0; i < budget; ++i) {
        wasmos_ipc_message_t msg;
        if (wasmos_ipc_try_recv(loop->receiver_endpoint) <= 0) {
            break;
        }
        wasmos_ipc_message_read_last(&msg);
        handled++;
        for (int32_t j = 0; j < WASMOS_SYS_INTENT_MAX; ++j) {
            if (loop->intents[j].in_use && loop->intents[j].request_id == msg.request_id) {
                void (*cb)(void *user, const wasmos_ipc_message_t *msg) = loop->intents[j].on_resolve;
                void *cb_user = loop->intents[j].user;
                loop->intents[j].in_use = 0;
                loop->intents[j].request_id = 0;
                loop->intents[j].on_resolve = 0;
                loop->intents[j].user = 0;
                cb(cb_user, &msg);
                goto wasmos_sys_event_loop_poll_done_message;
            }
        }
        int32_t dispatched = 0;
        for (int32_t j = 0; j < WASMOS_SYS_HANDLER_MAX; ++j) {
            if (loop->handlers[j].in_use &&
                loop->handlers[j].msg_type == msg.type &&
                loop->handlers[j].on_message) {
                loop->handlers[j].on_message(loop->handlers[j].user, &msg);
                dispatched = 1;
                break;
            }
        }
        if (!dispatched && loop->default_on_message) {
            loop->default_on_message(loop->default_user, &msg);
        }
wasmos_sys_event_loop_poll_done_message:
        ;
    }
    return handled;
}

static inline void
wasmos_sys_ipc_pack_name16(const char *name, int32_t out_args[4])
{
    wasmos_ipc_pack_name16(name, out_args);
}

static inline void
wasmos_sys_ipc_unpack_name16(uint32_t arg0,
                             uint32_t arg1,
                             uint32_t arg2,
                             uint32_t arg3,
                             char *out,
                             uint32_t out_len)
{
    uint32_t args[4] = { arg0, arg1, arg2, arg3 };
    uint32_t pos = 0;
    if (!out || out_len == 0) {
        return;
    }
    for (uint32_t i = 0; i < 4 && pos + 1 < out_len; ++i) {
        uint32_t v = args[i];
        for (uint32_t b = 0; b < 4 && pos + 1 < out_len; ++b) {
            char c = (char)(v & 0xFFu);
            if (c == '\0') {
                out[pos] = '\0';
                return;
            }
            out[pos++] = c;
            v >>= 8u;
        }
    }
    out[pos] = '\0';
}

static inline void
wasmos_sys_ipc_recv_loop(void)
{
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_recv(endpoint);
        }
    }
}

static inline int32_t
wasmos_sys_svc_lookup_retry(int32_t proc_endpoint,
                            int32_t reply_endpoint,
                            const char *service_name,
                            int32_t request_id_base,
                            int32_t attempts)
{
    if (attempts <= 0) {
        attempts = 1;
    }
    for (int32_t i = 0; i < attempts; ++i) {
        int32_t endpoint = wasmos_svc_lookup(proc_endpoint,
                                             reply_endpoint,
                                             service_name,
                                             request_id_base + i);
        if (endpoint >= 0) {
            return endpoint;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

static inline int32_t
wasmos_sys_ipc_send_retry(int32_t destination_endpoint,
                          int32_t source_endpoint,
                          int32_t type,
                          int32_t request_id,
                          int32_t arg0,
                          int32_t arg1,
                          int32_t arg2,
                          int32_t arg3,
                          int32_t retries)
{
    /* Keep in sync with kernel ipc.h */
    const int32_t ipc_err_full = -3;
    int32_t tries = 0;
    if (retries <= 0) {
        retries = 1;
    }
    for (;;) {
        int32_t rc = wasmos_ipc_send(destination_endpoint,
                                     source_endpoint,
                                     type,
                                     request_id,
                                     arg0,
                                     arg1,
                                     arg2,
                                     arg3);
        if (rc == 0 || rc != ipc_err_full) {
            return rc;
        }
        if (++tries >= retries) {
            return ipc_err_full;
        }
        (void)wasmos_sched_yield();
    }
}

static inline int32_t
wasmos_sys_buffer_copy_from(int32_t kind,
                            int32_t source_endpoint,
                            int32_t borrow_flags,
                            void *dst,
                            int32_t len,
                            int32_t offset)
{
    if (!dst || len < 0 || offset < 0) {
        return -1;
    }
    if (wasmos_buffer_borrow(kind, source_endpoint, borrow_flags) != 0) {
        return -1;
    }
    if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)dst, len, offset) != 0) {
        (void)wasmos_buffer_release(kind);
        return -1;
    }
    if (wasmos_buffer_release(kind) != 0) {
        return -1;
    }
    return 0;
}

static inline int32_t
wasmos_sys_buffer_write_to(int32_t kind,
                           int32_t source_endpoint,
                           int32_t borrow_flags,
                           const void *src,
                           int32_t len,
                           int32_t offset)
{
    if (!src || len < 0 || offset < 0) {
        return -1;
    }
    if (wasmos_buffer_borrow(kind, source_endpoint, borrow_flags) != 0) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)src, len, offset) != 0) {
        (void)wasmos_buffer_release(kind);
        return -1;
    }
    if (wasmos_buffer_release(kind) != 0) {
        return -1;
    }
    return 0;
}

static inline int32_t
wasmos_sys_fs_read_path(int32_t fs_endpoint,
                        int32_t reply_endpoint,
                        int32_t request_id,
                        const char *path,
                        char *out_text,
                        int32_t out_text_len)
{
    wasmos_ipc_message_t resp;
    int32_t path_len = 0;
    int32_t read_len = 0;
    if (!path || !out_text || out_text_len < 2) {
        return -1;
    }
    path_len = (int32_t)strlen(path);
    if (path_len <= 0 || path_len >= wasmos_fs_buffer_size()) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, path_len, 0) != 0) {
        return -1;
    }
    if (wasmos_ipc_send(fs_endpoint,
                        reply_endpoint,
                        FS_IPC_READ_PATH_REQ,
                        request_id,
                        path_len,
                        0,
                        0,
                        0) != 0) {
        return -1;
    }
    if (wasmos_sys_ipc_recv_matching(reply_endpoint, request_id, &resp) != 0) {
        return -1;
    }
    if (resp.type != FS_IPC_RESP) {
        return -1;
    }
    read_len = resp.arg0;
    if (read_len < 0) {
        return -1;
    }
    if (read_len >= out_text_len) {
        read_len = out_text_len - 1;
    }
    if (read_len > 0 &&
        wasmos_fs_buffer_copy((int32_t)(uintptr_t)out_text, read_len, 0) != 0) {
        return -1;
    }
    out_text[read_len] = '\0';
    return read_len;
}

static inline int32_t
wasmos_sys_fs_buffer_copy_from_endpoint(int32_t source_endpoint,
                                        void *dst,
                                        int32_t len,
                                        int32_t offset)
{
    return wasmos_sys_buffer_copy_from(WASMOS_BUFFER_KIND_FS,
                                       source_endpoint,
                                       WASMOS_BUFFER_GRANT_READ,
                                       dst,
                                       len,
                                       offset);
}

static inline int32_t
wasmos_sys_fs_buffer_write_to_endpoint(int32_t source_endpoint,
                                       const void *src,
                                       int32_t len,
                                       int32_t offset)
{
    return wasmos_sys_buffer_write_to(WASMOS_BUFFER_KIND_FS,
                                      source_endpoint,
                                      WASMOS_BUFFER_GRANT_WRITE,
                                      src,
                                      len,
                                      offset);
}

#ifdef __cplusplus
}
#endif

#endif
