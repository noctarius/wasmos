/* libsys_native.c - libsys_native implementation for Zig/C native drivers.
 * Thin wrappers around the wasmos_driver_api_t function-pointer table declared
 * in wasmos_native_driver.h (the native counterpart of the WASM hostcall
 * imports); compiled as C so both Zig and C native drivers link one object. */
#include "wasmos/libsys_native.h"
#include <string.h> /* libc str_copy_bytes (native drivers link libc string.c) */

static void byte_copy(uint8_t* dst, const uint8_t* src, uint32_t len) {
    uint32_t i = 0;
    if (!dst || !src) {
        return;
    }
    for (i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
}

static void byte_zero(uint8_t* dst, uint32_t len) {
    uint32_t i = 0;
    if (!dst) {
        return;
    }
    for (i = 0; i < len; ++i) {
        dst[i] = 0u;
    }
}

void wasmos_sys_byte_copy_native(uint8_t* dst, const uint8_t* src, uint32_t len) {
    byte_copy(dst, src, len);
}

int32_t wasmos_sys_be_u16_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 uint16_t* out) {
    if (!data || !out || off + 2u > data_len) {
        return -1;
    }
    *out = (uint16_t)(((uint16_t)data[off] << 8u) | (uint16_t)data[off + 1u]);
    return 0;
}

int32_t wasmos_sys_be_i16_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 int16_t* out) {
    uint16_t u = 0;
    if (!out || wasmos_sys_be_u16_native(data, data_len, off, &u) != 0) {
        return -1;
    }
    *out = (int16_t)u;
    return 0;
}

int32_t wasmos_sys_be_u32_native(const uint8_t* data, uint32_t data_len, uint32_t off,
                                 uint32_t* out) {
    if (!data || !out || off + 4u > data_len) {
        return -1;
    }
    *out = ((uint32_t)data[off] << 24u) | ((uint32_t)data[off + 1u] << 16u) |
           ((uint32_t)data[off + 2u] << 8u) | (uint32_t)data[off + 3u];
    return 0;
}

/* Search an OpenType/TrueType font binary for a table by 4-char tag;
 * returns 0 and sets *out_offset to the table's file offset, -1 if not found. */
int32_t wasmos_sys_find_table_native(const uint8_t* data, uint32_t data_len, const uint8_t tag[4],
                                     uint32_t* out_offset) {
    uint16_t num_tables = 0;
    uint32_t i = 0;
    if (!data || !tag || !out_offset) {
        return -1;
    }
    if (wasmos_sys_be_u16_native(data, data_len, 4u, &num_tables) != 0) {
        return -1;
    }
    for (i = 0; i < (uint32_t)num_tables; ++i) {
        uint32_t rec = 12u + i * 16u;
        uint32_t offset = 0;
        if (rec + 16u > data_len) {
            return -1;
        }
        if (data[rec] != tag[0] || data[rec + 1u] != tag[1] || data[rec + 2u] != tag[2] ||
            data[rec + 3u] != tag[3]) {
            continue;
        }
        if (wasmos_sys_be_u32_native(data, data_len, rec + 8u, &offset) != 0) {
            return -1;
        }
        *out_offset = offset;
        return 0;
    }
    return -1;
}

uint32_t wasmos_sys_pack_u16_pair_native(uint32_t a, uint32_t b) {
    uint16_t a16 = (uint16_t)(a & 0xFFFFu);
    uint16_t b16 = (uint16_t)(b & 0xFFFFu);
    return (uint32_t)a16 | ((uint32_t)b16 << 16u);
}

uint32_t wasmos_sys_pack_s16_pair_native(int32_t a, int32_t b) {
    uint16_t a16 = (uint16_t)(int16_t)a;
    uint16_t b16 = (uint16_t)(int16_t)b;
    return (uint32_t)a16 | ((uint32_t)b16 << 16u);
}

uint32_t wasmos_sys_hex_u32_native(uint32_t value, uint8_t* out, uint32_t out_len) {
    static const char* hex = "0123456789abcdef";
    uint32_t i = 0;
    if (!out || out_len < 11u) {
        return 0;
    }
    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 8u; ++i) {
        uint32_t shift = (7u - i) * 4u;
        out[2u + i] = (uint8_t)hex[(value >> shift) & 0xFu];
    }
    out[10] = '\0';
    return 10u;
}

/* Pack up to 16 bytes of a service name into four uint32 IPC args
 * (4 bytes each, little-endian). */
void wasmos_sys_ipc_pack_name16_native(const uint8_t* name, uint32_t name_len,
                                       uint32_t out_args[4]) {
    uint32_t i = 0;
    if (!out_args) {
        return;
    }
    out_args[0] = 0;
    out_args[1] = 0;
    out_args[2] = 0;
    out_args[3] = 0;
    if (!name) {
        return;
    }
    for (i = 0; i < name_len && i < 16u; ++i) {
        uint32_t slot = i / 4u;
        uint32_t shift = (i % 4u) * 8u;
        out_args[slot] |= ((uint32_t)name[i]) << shift;
    }
}

void wasmos_sys_ipc_unpack_name16_native(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                         uint8_t* out, uint32_t out_len) {
    uint32_t args[4];
    uint32_t i = 0;
    uint32_t pos = 0;
    if (!out || out_len == 0u) {
        return;
    }
    args[0] = arg0;
    args[1] = arg1;
    args[2] = arg2;
    args[3] = arg3;
    for (i = 0; i < 4u && pos + 1u < out_len; ++i) {
        uint32_t v = args[i];
        uint32_t b = 0;
        for (b = 0; b < 4u && pos + 1u < out_len; ++b) {
            uint8_t c = (uint8_t)(v & 0xFFu);
            if (c == 0u) {
                out[pos] = 0u;
                return;
            }
            out[pos++] = c;
            v >>= 8u;
        }
    }
    out[pos] = 0u;
}

/* Terminal-state parking loop for native drivers: never returns, drains and
 * DISCARDS anything sent to receiver_endpoint, and yields whenever the endpoint
 * reports IPC_EMPTY (rc == 1) so other processes run.
 * FIXME: this yield-spins instead of blocking; a driver parked here still costs
 * a scheduling slot per pass. Use api->ipc_wait once callers can be migrated. */
void wasmos_sys_ipc_recv_loop_native(wasmos_driver_api_t* api, uint32_t receiver_endpoint) {
    nd_ipc_message_t msg;
    if (!api || !api->ipc_recv || !api->sched_current_pid) {
        return;
    }
    for (;;) {
        uint32_t ctx_id = api->sched_current_pid();
        int32_t rc = api->ipc_recv(ctx_id, receiver_endpoint, &msg);
        if (rc == 1 && api->sched_yield) {
            api->sched_yield();
        }
    }
}

static wasmos_sys_native_intent_t* native_intent_find(wasmos_sys_native_event_loop_t* loop,
                                                      uint32_t request_id) {
    uint32_t i = 0;
    if (!loop) {
        return 0;
    }
    for (i = 0; i < WASMOS_SYS_NATIVE_INTENT_MAX; ++i) {
        if (loop->intents[i].in_use && loop->intents[i].request_id == request_id) {
            return &loop->intents[i];
        }
    }
    return 0;
}

static wasmos_sys_native_intent_t* native_intent_alloc(wasmos_sys_native_event_loop_t* loop) {
    uint32_t i = 0;
    if (!loop) {
        return 0;
    }
    for (i = 0; i < WASMOS_SYS_NATIVE_INTENT_MAX; ++i) {
        if (!loop->intents[i].in_use) {
            return &loop->intents[i];
        }
    }
    return 0;
}

void wasmos_sys_native_event_loop_init(wasmos_sys_native_event_loop_t* loop,
                                       wasmos_driver_api_t* api, uint32_t receiver_endpoint,
                                       uint32_t request_id_base) {
    uint32_t i = 0;
    if (!loop) {
        return;
    }
    loop->api = api;
    loop->receiver_endpoint = receiver_endpoint;
    loop->next_request_id = request_id_base;
    loop->default_on_message = 0;
    loop->default_user = 0;
    for (i = 0; i < WASMOS_SYS_NATIVE_INTENT_MAX; ++i) {
        loop->intents[i].in_use = 0;
        loop->intents[i].request_id = 0;
        loop->intents[i].on_resolve = 0;
        loop->intents[i].user = 0;
    }
    for (i = 0; i < WASMOS_SYS_NATIVE_HANDLER_MAX; ++i) {
        loop->handlers[i].in_use = 0;
        loop->handlers[i].msg_type = 0;
        loop->handlers[i].on_message = 0;
        loop->handlers[i].user = 0;
    }
}

int32_t wasmos_sys_native_event_set_default(wasmos_sys_native_event_loop_t* loop,
                                            void (*on_message)(void* user,
                                                               const nd_ipc_message_t* msg),
                                            void* user) {
    if (!loop || !on_message) {
        return -1;
    }
    loop->default_on_message = on_message;
    loop->default_user = user;
    return 0;
}

int32_t wasmos_sys_native_event_register(wasmos_sys_native_event_loop_t* loop, uint32_t msg_type,
                                         void (*on_message)(void* user,
                                                            const nd_ipc_message_t* msg),
                                         void* user) {
    uint32_t i = 0;
    if (!loop || !on_message) {
        return -1;
    }
    for (i = 0; i < WASMOS_SYS_NATIVE_HANDLER_MAX; ++i) {
        if (loop->handlers[i].in_use && loop->handlers[i].msg_type == msg_type) {
            loop->handlers[i].on_message = on_message;
            loop->handlers[i].user = user;
            return 0;
        }
    }
    for (i = 0; i < WASMOS_SYS_NATIVE_HANDLER_MAX; ++i) {
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

int32_t wasmos_sys_native_intent_send(wasmos_sys_native_event_loop_t* loop,
                                      uint32_t destination_endpoint, uint32_t source_endpoint,
                                      uint32_t msg_type, uint32_t arg0, uint32_t arg1,
                                      uint32_t arg2, uint32_t arg3,
                                      void (*on_resolve)(void* user, const nd_ipc_message_t* msg),
                                      void* user, uint32_t* out_request_id) {
    nd_ipc_message_t req;
    wasmos_sys_native_intent_t* slot = 0;
    uint32_t ctx_id = 0;
    int32_t send_rc = 0;
    if (!loop || !loop->api || !loop->api->ipc_send || !loop->api->sched_current_pid ||
        !on_resolve) {
        return -1;
    }
    slot = native_intent_alloc(loop);
    if (!slot) {
        return -1;
    }
    req.type = msg_type;
    req.source = source_endpoint;
    req.destination = destination_endpoint;
    req.request_id = loop->next_request_id++;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.arg3 = arg3;
    ctx_id = loop->api->sched_current_pid();
    send_rc = loop->api->ipc_send(ctx_id, destination_endpoint, &req);
    if (send_rc != 0) {
        return send_rc;
    }
    slot->in_use = 1;
    slot->request_id = req.request_id;
    slot->on_resolve = on_resolve;
    slot->user = user;
    if (out_request_id) {
        *out_request_id = req.request_id;
    }
    return 0;
}

int32_t wasmos_sys_native_intent_send_with_request_id(
    wasmos_sys_native_event_loop_t* loop, uint32_t destination_endpoint, uint32_t source_endpoint,
    uint32_t request_id, uint32_t msg_type, uint32_t arg0, uint32_t arg1, uint32_t arg2,
    uint32_t arg3, void (*on_resolve)(void* user, const nd_ipc_message_t* msg), void* user) {
    nd_ipc_message_t req;
    wasmos_sys_native_intent_t* slot = 0;
    uint32_t ctx_id = 0;
    int32_t send_rc = 0;
    if (!loop || !loop->api || !loop->api->ipc_send || !loop->api->sched_current_pid ||
        !on_resolve || request_id == 0) {
        return -1;
    }
    if (native_intent_find(loop, request_id)) {
        return -1;
    }
    slot = native_intent_alloc(loop);
    if (!slot) {
        return -1;
    }
    req.type = msg_type;
    req.source = source_endpoint;
    req.destination = destination_endpoint;
    req.request_id = request_id;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.arg3 = arg3;
    ctx_id = loop->api->sched_current_pid();
    send_rc = loop->api->ipc_send(ctx_id, destination_endpoint, &req);
    if (send_rc != 0) {
        return send_rc;
    }
    slot->in_use = 1;
    slot->request_id = req.request_id;
    slot->on_resolve = on_resolve;
    slot->user = user;
    return 0;
}

void wasmos_sys_native_intent_cancel(wasmos_sys_native_event_loop_t* loop, uint32_t request_id) {
    wasmos_sys_native_intent_t* intent = native_intent_find(loop, request_id);
    if (!intent) {
        return;
    }
    intent->in_use = 0;
    intent->request_id = 0;
    intent->on_resolve = 0;
    intent->user = 0;
}

static void native_random_finish(wasmos_sys_native_random_request_t* request, int32_t status) {
    if (!request) {
        return;
    }
    if (request->buffer_id != 0u && request->loop && request->loop->api &&
        request->loop->api->xfer_buffer_release) {
        (void)request->loop->api->xfer_buffer_release(request->buffer_id);
        request->buffer_id = 0u;
    }
    if (status == 0 && request->float_out) {
        *request->float_out = (float)(request->float_word >> 8u) * (1.0f / 16777216.0f);
    }
    if (request->on_complete) {
        request->on_complete(request->user, status);
    }
}

static int32_t native_random_issue(wasmos_sys_native_random_request_t* request);

static void native_random_reply(void* user, const nd_ipc_message_t* reply) {
    wasmos_sys_native_random_request_t* request = (wasmos_sys_native_random_request_t*)user;
    int32_t wrote;
    if (!request || !reply) {
        return;
    }
    if (reply->type == HRNG_IPC_ERROR) {
        native_random_finish(
            request, (int32_t)reply->arg0 < 0 ? (int32_t)reply->arg0 : WASMOS_ERR_HRNG_IO_ERROR);
        return;
    }
    if (reply->type != HRNG_IPC_RESP) {
        native_random_finish(request, WASMOS_ERR_HRNG_PROTOCOL);
        return;
    }
    wrote = (int32_t)reply->arg0;
    if (wrote <= 0 || (uint32_t)wrote > request->chunk_max ||
        (uint32_t)wrote > request->len - request->done) {
        native_random_finish(request, WASMOS_ERR_HRNG_IO_ERROR);
        return;
    }
    byte_copy(request->out + request->done, request->buffer, (uint32_t)wrote);
    request->done += (uint32_t)wrote;
    if (request->done == request->len) {
        native_random_finish(request, 0);
        return;
    }
    if (native_random_issue(request) != 0) {
        native_random_finish(request, WASMOS_ERR_HRNG_IO_ERROR);
    }
}

static int32_t native_random_issue(wasmos_sys_native_random_request_t* request) {
    uint32_t chunk;
    if (!request || !request->loop || request->done >= request->len) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    chunk = request->len - request->done;
    if (chunk > request->chunk_max) {
        chunk = request->chunk_max;
    }
    return wasmos_sys_native_intent_send(request->loop,
                                         request->hrng_endpoint,
                                         request->loop->receiver_endpoint,
                                         HRNG_IPC_GET_BYTES_REQ,
                                         request->buffer_id,
                                         chunk,
                                         0u,
                                         0u,
                                         native_random_reply,
                                         request,
                                         0);
}

int32_t wasmos_sys_native_random_bytes_async(wasmos_sys_native_event_loop_t* loop,
                                             uint32_t hrng_endpoint, uint8_t* out, uint32_t len,
                                             wasmos_sys_native_random_request_t* request,
                                             wasmos_sys_native_random_complete_fn on_complete,
                                             void* user) {
    if (!loop || !loop->api || !loop->api->xfer_buffer_acquire || !loop->api->xfer_buffer_borrow ||
        !loop->api->xfer_buffer_release || !request || !out || len == 0u ||
        hrng_endpoint == WASMOS_SYS_NATIVE_ENDPOINT_NONE ||
        loop->receiver_endpoint == WASMOS_SYS_NATIVE_ENDPOINT_NONE || !on_complete) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    request->loop = loop;
    request->hrng_endpoint = hrng_endpoint;
    request->buffer_id = 0u;
    request->chunk_max = HRNG_MAX_BYTES_PER_REQ;
    request->len = len;
    request->done = 0u;
    request->buffer = 0;
    request->out = out;
    request->float_out = 0;
    request->on_complete = on_complete;
    request->user = user;
    request->buffer = (uint8_t*)loop->api->xfer_buffer_acquire(
        ND_BUFFER_KIND_XFER, request->chunk_max, &request->buffer_id);
    if (!request->buffer || request->buffer_id == 0u ||
        loop->api->xfer_buffer_borrow(hrng_endpoint, request->buffer_id, ND_BUFFER_BORROW_WRITE) <
            0) {
        if (request->buffer_id != 0u) {
            (void)loop->api->xfer_buffer_release(request->buffer_id);
            request->buffer_id = 0u;
        }
        return WASMOS_ERR_HRNG_NOT_READY;
    }
    if (native_random_issue(request) != 0) {
        (void)loop->api->xfer_buffer_release(request->buffer_id);
        request->buffer_id = 0u;
        return WASMOS_ERR_HRNG_IO_ERROR;
    }
    return 0;
}

int32_t wasmos_sys_native_random_int_async(wasmos_sys_native_event_loop_t* loop,
                                           uint32_t hrng_endpoint, uint32_t* out_value,
                                           wasmos_sys_native_random_request_t* request,
                                           wasmos_sys_native_random_complete_fn on_complete,
                                           void* user) {
    if (!request) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    request->float_out = 0;
    return wasmos_sys_native_random_bytes_async(loop,
                                                hrng_endpoint,
                                                (uint8_t*)out_value,
                                                (uint32_t)sizeof(*out_value),
                                                request,
                                                on_complete,
                                                user);
}

int32_t wasmos_sys_native_random_float_async(wasmos_sys_native_event_loop_t* loop,
                                             uint32_t hrng_endpoint, float* out_value,
                                             wasmos_sys_native_random_request_t* request,
                                             wasmos_sys_native_random_complete_fn on_complete,
                                             void* user) {
    if (!request || !out_value) {
        return WASMOS_ERR_HRNG_INVALID;
    }
    int32_t status = wasmos_sys_native_random_bytes_async(loop,
                                                          hrng_endpoint,
                                                          (uint8_t*)&request->float_word,
                                                          (uint32_t)sizeof(request->float_word),
                                                          request,
                                                          on_complete,
                                                          user);
    if (status != WASMOS_ERR_NONE) {
        return status;
    }
    request->float_out = out_value;
    return WASMOS_ERR_NONE;
}

int32_t wasmos_sys_native_event_loop_poll(wasmos_sys_native_event_loop_t* loop, uint32_t budget) {
    uint32_t ctx_id = 0;
    uint32_t i = 0;
    uint32_t handled = 0;
    if (!loop || !loop->api || !loop->api->ipc_recv || !loop->api->sched_current_pid) {
        return -1;
    }
    if (budget == 0u) {
        budget = 1u;
    }
    ctx_id = loop->api->sched_current_pid();
    for (i = 0; i < budget; ++i) {
        nd_ipc_message_t msg;
        int32_t rc = loop->api->ipc_recv(ctx_id, loop->receiver_endpoint, &msg);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            return -1;
        }
        handled++;
        wasmos_sys_native_intent_t* intent = native_intent_find(loop, msg.request_id);
        if (intent) {
            void (*cb)(void*, const nd_ipc_message_t*) = intent->on_resolve;
            void* user = intent->user;
            intent->in_use = 0;
            intent->request_id = 0;
            intent->on_resolve = 0;
            intent->user = 0;
            cb(user, &msg);
            continue;
        }
        uint8_t dispatched = 0;
        for (uint32_t h = 0; h < WASMOS_SYS_NATIVE_HANDLER_MAX; ++h) {
            if (loop->handlers[h].in_use && loop->handlers[h].msg_type == msg.type &&
                loop->handlers[h].on_message) {
                loop->handlers[h].on_message(loop->handlers[h].user, &msg);
                dispatched = 1;
                break;
            }
        }
        if (!dispatched && loop->default_on_message) {
            loop->default_on_message(loop->default_user, &msg);
        }
    }
    return (int32_t)handled;
}

int32_t wasmos_sys_ipc_recv_matching_native(wasmos_driver_api_t* api, uint32_t receiver_endpoint,
                                            uint32_t request_id, nd_ipc_message_t* out_message) {
    uint32_t ctx_id = 0;
    int32_t rc = 0;
    if (!api || !out_message || !api->ipc_recv || !api->sched_current_pid) {
        return -1;
    }
    ctx_id = api->sched_current_pid();
    for (;;) {
        rc = api->ipc_recv(ctx_id, receiver_endpoint, out_message);
        if (rc == 1) {
            if (api->sched_yield) {
                api->sched_yield();
            }
            continue;
        }
        if (rc != 0) {
            return -1;
        }
        if (out_message->request_id == request_id) {
            return 0;
        }
    }
}

/* Retries only IPC_ERR_FULL (-3, kernel include/ipc.h): a momentarily full
 * destination queue. Any other status is returned to the caller unchanged. */
int32_t wasmos_sys_ipc_send_retry_native(wasmos_driver_api_t* api, uint32_t destination_endpoint,
                                         uint32_t source_endpoint, uint32_t msg_type,
                                         uint32_t request_id, uint32_t arg0, uint32_t arg1,
                                         uint32_t arg2, uint32_t arg3, uint32_t retries) {
    nd_ipc_message_t req;
    uint32_t ctx_id = 0;
    uint32_t tries = 0;
    if (!api || !api->ipc_send || !api->sched_current_pid) {
        return -1;
    }
    if (retries == 0u) {
        retries = 1u;
    }
    req.type = msg_type;
    req.source = source_endpoint;
    req.destination = destination_endpoint;
    req.request_id = request_id;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.arg3 = arg3;
    ctx_id = api->sched_current_pid();
    for (;;) {
        int32_t rc = api->ipc_send(ctx_id, destination_endpoint, &req);
        if (rc == 0) {
            return 0;
        }
        if (rc != -3) {
            return rc;
        }
        if (++tries >= retries) {
            return -3;
        }
        if (api->sched_yield) {
            api->sched_yield();
        }
    }
}

/* Synchronous send+recv: sends the request and blocks in
 * wasmos_sys_ipc_recv_matching_native until the matching reply arrives.
 * Returns 0 on success, -1 on error. */
int32_t wasmos_sys_ipc_call_native(wasmos_driver_api_t* api, uint32_t source_endpoint,
                                   uint32_t destination, uint32_t request_id, uint32_t msg_type,
                                   uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                   nd_ipc_message_t* out_message) {
    nd_ipc_message_t req;
    uint32_t ctx_id = 0;
    if (!api || !out_message || !api->ipc_send || !api->sched_current_pid) {
        return -1;
    }
    req.type = msg_type;
    req.source = source_endpoint;
    req.destination = destination;
    req.request_id = request_id;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.arg3 = arg3;
    ctx_id = api->sched_current_pid();
    if (api->ipc_send(ctx_id, destination, &req) != 0) {
        return -1;
    }
    return wasmos_sys_ipc_recv_matching_native(api, source_endpoint, request_id, out_message);
}

int32_t wasmos_sys_net_resolve_native(wasmos_driver_api_t* api, uint32_t source_endpoint,
                                      uint32_t stack_endpoint, const char* hostname,
                                      uint32_t hostname_len, uint32_t request_id,
                                      uint32_t* out_addr_no) {
    nd_ipc_message_t reply;
    uint8_t* buf;
    uint32_t buffer_id = 0;
    int32_t grant;
    int32_t rc;
    if (!api || !api->xfer_buffer_acquire || !api->xfer_buffer_borrow ||
        !api->xfer_buffer_release || !hostname || hostname_len == 0u) {
        return -1;
    }
    buf = (uint8_t*)api->xfer_buffer_acquire(ND_BUFFER_KIND_XFER, hostname_len, &buffer_id);
    if (!buf) {
        return -1;
    }
    byte_copy(buf, (const uint8_t*)hostname, hostname_len);
    /* Borrow the name read-only to net-stack; it maps, copies, and unmaps before
     * it can reply, so the buffer is safe to release once the reply arrives. */
    grant = api->xfer_buffer_borrow(stack_endpoint, buffer_id, ND_BUFFER_BORROW_READ);
    if (grant < 0) {
        (void)api->xfer_buffer_release(buffer_id);
        return -1;
    }
    rc = wasmos_sys_ipc_call_native(api,
                                    source_endpoint,
                                    stack_endpoint,
                                    request_id,
                                    NET_IPC_RESOLVE,
                                    buffer_id,
                                    (uint32_t)grant,
                                    hostname_len,
                                    0u,
                                    &reply);
    (void)api->xfer_buffer_release(buffer_id);
    if (rc != 0 || reply.type != NET_IPC_RESP || (int32_t)reply.arg0 != WASMOS_ERR_NONE) {
        return -1;
    }
    if (out_addr_no) {
        *out_addr_no = reply.arg1;
    }
    return 0;
}

int32_t wasmos_sys_svc_register_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                       uint32_t source_endpoint, const uint8_t* name,
                                       uint32_t name_len, uint32_t request_id) {
    nd_ipc_message_t msg;
    uint32_t args[4];
    uint32_t ctx_id = 0;
    if (!api || !api->ipc_send || !api->sched_current_pid) {
        return -1;
    }
    wasmos_sys_ipc_pack_name16_native(name, name_len, args);
    msg.type = SVC_IPC_REGISTER_REQ;
    msg.source = source_endpoint;
    msg.destination = proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];
    ctx_id = api->sched_current_pid();
    if (api->ipc_send(ctx_id, proc_endpoint, &msg) != 0) {
        return -1;
    }
    if (wasmos_sys_ipc_recv_matching_native(api, source_endpoint, request_id, &msg) != 0) {
        return -1;
    }
    if (msg.type != SVC_IPC_REGISTER_RESP) {
        return -1;
    }
    return (int32_t)msg.arg0;
}

int32_t wasmos_sys_svc_lookup_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                     uint32_t source_endpoint, const uint8_t* name,
                                     uint32_t name_len, uint32_t request_id) {
    nd_ipc_message_t msg;
    uint32_t args[4];
    uint32_t ctx_id = 0;
    if (!api || !api->ipc_send || !api->sched_current_pid) {
        return -1;
    }
    wasmos_sys_ipc_pack_name16_native(name, name_len, args);
    msg.type = SVC_IPC_LOOKUP_REQ;
    msg.source = source_endpoint;
    msg.destination = proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];
    ctx_id = api->sched_current_pid();
    if (api->ipc_send(ctx_id, proc_endpoint, &msg) != 0) {
        return -1;
    }
    if (wasmos_sys_ipc_recv_matching_native(api, source_endpoint, request_id, &msg) != 0) {
        return -1;
    }
    if (msg.type != SVC_IPC_LOOKUP_RESP || msg.arg0 == 0xFFFFFFFFu) {
        return -1;
    }
    return (int32_t)msg.arg0;
}

int32_t wasmos_sys_svc_lookup_retry_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                           uint32_t source_endpoint, const uint8_t* name,
                                           uint32_t name_len, uint32_t request_id_base,
                                           uint32_t attempts) {
    uint32_t i = 0;
    if (attempts == 0u) {
        attempts = 1u;
    }
    for (i = 0; i < attempts; ++i) {
        int32_t ep = wasmos_sys_svc_lookup_native(
            api, proc_endpoint, source_endpoint, name, name_len, request_id_base + i);
        if (ep >= 0) {
            return ep;
        }
        if (api && api->sched_yield) {
            api->sched_yield();
        }
    }
    return -1;
}

/* Register service_endpoint under a name and (optionally) a virtual class +
 * instance, using the descriptor-based SVC_IPC_REGISTER_DESC_REQ. Pass
 * class_name=NULL/class_len=0 to register with no class. Returns the reply's
 * arg0, which the process manager always sets to 0, or -1 on failure.
 * Blocking: waits on source_endpoint for the SVC_IPC_REGISTER_RESP. Names and
 * class names that exceed WASMOS_SVC_NAME_MAX / WASMOS_SVC_CLASS_MAX are
 * REFUSED, not truncated, and the descriptor field is left empty. The
 * descriptor's transfer buffer is released on every exit path after acquire. */
int32_t wasmos_sys_svc_register_class_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                             uint32_t source_endpoint, uint32_t service_endpoint,
                                             const uint8_t* name, uint32_t name_len,
                                             const uint8_t* class_name, uint32_t class_len,
                                             uint32_t instance, uint32_t request_id) {
    return wasmos_sys_svc_register_class_flags_native(api,
                                                      proc_endpoint,
                                                      source_endpoint,
                                                      service_endpoint,
                                                      name,
                                                      name_len,
                                                      class_name,
                                                      class_len,
                                                      instance,
                                                      0u,
                                                      request_id);
}

int32_t wasmos_sys_svc_register_class_flags_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                                   uint32_t source_endpoint,
                                                   uint32_t service_endpoint, const uint8_t* name,
                                                   uint32_t name_len, const uint8_t* class_name,
                                                   uint32_t class_len, uint32_t instance,
                                                   uint32_t flags, uint32_t request_id) {
    svc_register_desc_t* desc;
    nd_ipc_message_t resp;
    uint32_t buffer_id = 0;
    int32_t rc;
    if (!api || !api->xfer_buffer_acquire || !api->xfer_buffer_release) {
        return -1;
    }
    desc = (svc_register_desc_t*)api->xfer_buffer_acquire(
        ND_BUFFER_KIND_XFER, (uint32_t)sizeof(*desc), &buffer_id);
    if (!desc) {
        return -1;
    }
    byte_zero((uint8_t*)desc, (uint32_t)sizeof(*desc));
    desc->version = WASMOS_SVC_REGISTER_DESC_VERSION;
    desc->service_endpoint = service_endpoint;
    desc->flags = flags & WASMOS_SVC_FLAG_MASK;
    /* desc is zeroed above, so name/class_name default to "" if the source is
     * over-long (str_copy_bytes refuses rather than truncating) or absent. */
    (void)str_copy_bytes(desc->name, WASMOS_SVC_NAME_MAX, name, name_len);
    desc->instance = instance;
    (void)str_copy_bytes(desc->class_name, WASMOS_SVC_CLASS_MAX, class_name, class_len);
    rc = wasmos_sys_ipc_call_native(api,
                                    source_endpoint,
                                    proc_endpoint,
                                    request_id,
                                    SVC_IPC_REGISTER_DESC_REQ,
                                    0u,
                                    (uint32_t)sizeof(*desc),
                                    buffer_id,
                                    0u,
                                    &resp);
    (void)api->xfer_buffer_release(buffer_id);
    if (rc != 0 || resp.type != SVC_IPC_REGISTER_RESP) {
        return -1;
    }
    return (int32_t)resp.arg0;
}

/* Enumerate providers of a virtual class into out[0..max_entries). Returns the
 * total match count (may exceed max_entries; only min(count,max_entries) entries
 * are written), or -1 on error. */
int32_t wasmos_sys_svc_lookup_class_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                           uint32_t source_endpoint, const uint8_t* class_name,
                                           uint32_t class_len, svc_class_entry_t* out,
                                           uint32_t max_entries, uint32_t request_id) {
    nd_ipc_message_t resp;
    uint8_t* buf;
    uint32_t buffer_id = 0;
    uint32_t sz;
    int32_t count;
    uint32_t got;
    if (!api || !api->xfer_buffer_acquire || !api->xfer_buffer_release) {
        return -1;
    }
    sz = max_entries * (uint32_t)sizeof(svc_class_entry_t);
    if (sz < WASMOS_SVC_CLASS_MAX) {
        sz = WASMOS_SVC_CLASS_MAX; /* room for the class name on input */
    }
    buf = (uint8_t*)api->xfer_buffer_acquire(ND_BUFFER_KIND_XFER, sz, &buffer_id);
    if (!buf) {
        return -1;
    }
    if (str_copy_bytes((char*)buf, WASMOS_SVC_CLASS_MAX, class_name, class_len) != 0) {
        buf[0] = '\0';
    }
    if (wasmos_sys_ipc_call_native(api,
                                   source_endpoint,
                                   proc_endpoint,
                                   request_id,
                                   SVC_IPC_LOOKUP_CLASS_REQ,
                                   buffer_id,
                                   max_entries,
                                   0u,
                                   0u,
                                   &resp) != 0 ||
        resp.type != SVC_IPC_LOOKUP_CLASS_RESP) {
        (void)api->xfer_buffer_release(buffer_id);
        return -1;
    }
    count = (int32_t)resp.arg0;
    got = ((uint32_t)count < max_entries) ? (uint32_t)count : max_entries;
    if (out && count > 0 && got > 0u) {
        byte_copy((uint8_t*)out, buf, got * (uint32_t)sizeof(svc_class_entry_t));
    }
    (void)api->xfer_buffer_release(buffer_id);
    return count;
}

/* Subscribe notify_endpoint to existence events (SVC_IPC_CLASS_EVENT) for a
 * class. Returns 0 on success, -1 on error. */
int32_t wasmos_sys_svc_subscribe_class_native(wasmos_driver_api_t* api, uint32_t proc_endpoint,
                                              uint32_t source_endpoint, uint32_t notify_endpoint,
                                              const uint8_t* class_name, uint32_t class_len,
                                              uint32_t request_id) {
    nd_ipc_message_t resp;
    uint8_t* buf;
    uint32_t buffer_id = 0;
    if (!api || !api->xfer_buffer_acquire || !api->xfer_buffer_release) {
        return -1;
    }
    buf = (uint8_t*)api->xfer_buffer_acquire(ND_BUFFER_KIND_XFER, WASMOS_SVC_CLASS_MAX, &buffer_id);
    if (!buf) {
        return -1;
    }
    if (str_copy_bytes((char*)buf, WASMOS_SVC_CLASS_MAX, class_name, class_len) != 0) {
        buf[0] = '\0';
    }
    if (wasmos_sys_ipc_call_native(api,
                                   source_endpoint,
                                   proc_endpoint,
                                   request_id,
                                   SVC_IPC_SUBSCRIBE_CLASS_REQ,
                                   notify_endpoint,
                                   buffer_id,
                                   0u,
                                   0u,
                                   &resp) != 0 ||
        resp.type != SVC_IPC_SUBSCRIBE_CLASS_RESP) {
        (void)api->xfer_buffer_release(buffer_id);
        return -1;
    }
    (void)api->xfer_buffer_release(buffer_id);
    return 0;
}
