#include "wasmos/libsys_native.h"

static void
byte_copy(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    uint32_t i = 0;
    if (!dst || !src) {
        return;
    }
    for (i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
}

void
wasmos_sys_byte_copy_native(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    byte_copy(dst, src, len);
}

int32_t
wasmos_sys_be_u16_native(const uint8_t *data, uint32_t data_len, uint32_t off, uint16_t *out)
{
    if (!data || !out || off + 2u > data_len) {
        return -1;
    }
    *out = (uint16_t)(((uint16_t)data[off] << 8u) | (uint16_t)data[off + 1u]);
    return 0;
}

int32_t
wasmos_sys_be_i16_native(const uint8_t *data, uint32_t data_len, uint32_t off, int16_t *out)
{
    uint16_t u = 0;
    if (!out || wasmos_sys_be_u16_native(data, data_len, off, &u) != 0) {
        return -1;
    }
    *out = (int16_t)u;
    return 0;
}

int32_t
wasmos_sys_be_u32_native(const uint8_t *data, uint32_t data_len, uint32_t off, uint32_t *out)
{
    if (!data || !out || off + 4u > data_len) {
        return -1;
    }
    *out = ((uint32_t)data[off] << 24u) |
           ((uint32_t)data[off + 1u] << 16u) |
           ((uint32_t)data[off + 2u] << 8u) |
           (uint32_t)data[off + 3u];
    return 0;
}

int32_t
wasmos_sys_find_table_native(const uint8_t *data, uint32_t data_len, const uint8_t tag[4], uint32_t *out_offset)
{
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
        if (data[rec] != tag[0] || data[rec + 1u] != tag[1] || data[rec + 2u] != tag[2] || data[rec + 3u] != tag[3]) {
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

uint32_t
wasmos_sys_pack_u16_pair_native(uint32_t a, uint32_t b)
{
    uint16_t a16 = (uint16_t)(a & 0xFFFFu);
    uint16_t b16 = (uint16_t)(b & 0xFFFFu);
    return (uint32_t)a16 | ((uint32_t)b16 << 16u);
}

uint32_t
wasmos_sys_pack_s16_pair_native(int32_t a, int32_t b)
{
    uint16_t a16 = (uint16_t)(int16_t)a;
    uint16_t b16 = (uint16_t)(int16_t)b;
    return (uint32_t)a16 | ((uint32_t)b16 << 16u);
}

uint32_t
wasmos_sys_hex_u32_native(uint32_t value, uint8_t *out, uint32_t out_len)
{
    static const char *hex = "0123456789abcdef";
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

void
wasmos_sys_ipc_pack_name16_native(const uint8_t *name, uint32_t name_len, uint32_t out_args[4])
{
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

void
wasmos_sys_ipc_unpack_name16_native(uint32_t arg0,
                                    uint32_t arg1,
                                    uint32_t arg2,
                                    uint32_t arg3,
                                    uint8_t *out,
                                    uint32_t out_len)
{
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

void
wasmos_sys_ipc_recv_loop_native(wasmos_driver_api_t *api, uint32_t receiver_endpoint)
{
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

static wasmos_sys_native_intent_t *
native_intent_find(wasmos_sys_native_event_loop_t *loop, uint32_t request_id)
{
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

static wasmos_sys_native_intent_t *
native_intent_alloc(wasmos_sys_native_event_loop_t *loop)
{
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

void
wasmos_sys_native_event_loop_init(wasmos_sys_native_event_loop_t *loop,
                                  wasmos_driver_api_t *api,
                                  uint32_t receiver_endpoint,
                                  uint32_t request_id_base)
{
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

int32_t
wasmos_sys_native_event_set_default(wasmos_sys_native_event_loop_t *loop,
                                    void (*on_message)(void *user, const nd_ipc_message_t *msg),
                                    void *user)
{
    if (!loop || !on_message) {
        return -1;
    }
    loop->default_on_message = on_message;
    loop->default_user = user;
    return 0;
}

int32_t
wasmos_sys_native_event_register(wasmos_sys_native_event_loop_t *loop,
                                 uint32_t msg_type,
                                 void (*on_message)(void *user, const nd_ipc_message_t *msg),
                                 void *user)
{
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

int32_t
wasmos_sys_native_intent_send(wasmos_sys_native_event_loop_t *loop,
                              uint32_t destination_endpoint,
                              uint32_t source_endpoint,
                              uint32_t msg_type,
                              uint32_t arg0,
                              uint32_t arg1,
                              uint32_t arg2,
                              uint32_t arg3,
                              void (*on_resolve)(void *user, const nd_ipc_message_t *msg),
                              void *user,
                              uint32_t *out_request_id)
{
    nd_ipc_message_t req;
    wasmos_sys_native_intent_t *slot = 0;
    uint32_t ctx_id = 0;
    int32_t send_rc = 0;
    if (!loop || !loop->api || !loop->api->ipc_send || !loop->api->sched_current_pid || !on_resolve) {
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

int32_t
wasmos_sys_native_intent_send_with_request_id(wasmos_sys_native_event_loop_t *loop,
                                              uint32_t destination_endpoint,
                                              uint32_t source_endpoint,
                                              uint32_t request_id,
                                              uint32_t msg_type,
                                              uint32_t arg0,
                                              uint32_t arg1,
                                              uint32_t arg2,
                                              uint32_t arg3,
                                              void (*on_resolve)(void *user, const nd_ipc_message_t *msg),
                                              void *user)
{
    nd_ipc_message_t req;
    wasmos_sys_native_intent_t *slot = 0;
    uint32_t ctx_id = 0;
    int32_t send_rc = 0;
    if (!loop || !loop->api || !loop->api->ipc_send || !loop->api->sched_current_pid || !on_resolve || request_id == 0) {
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

int32_t
wasmos_sys_native_event_loop_poll(wasmos_sys_native_event_loop_t *loop, uint32_t budget)
{
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
        {
            wasmos_sys_native_intent_t *intent = native_intent_find(loop, msg.request_id);
            if (intent) {
                void (*cb)(void *, const nd_ipc_message_t *) = intent->on_resolve;
                void *user = intent->user;
                intent->in_use = 0;
                intent->request_id = 0;
                intent->on_resolve = 0;
                intent->user = 0;
                cb(user, &msg);
                continue;
            }
        }
        uint8_t dispatched = 0;
        for (uint32_t h = 0; h < WASMOS_SYS_NATIVE_HANDLER_MAX; ++h) {
            if (loop->handlers[h].in_use &&
                loop->handlers[h].msg_type == msg.type &&
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

int32_t
wasmos_sys_ipc_recv_matching_native(wasmos_driver_api_t *api,
                                    uint32_t receiver_endpoint,
                                    uint32_t request_id,
                                    nd_ipc_message_t *out_message)
{
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

int32_t
wasmos_sys_ipc_send_retry_native(wasmos_driver_api_t *api,
                                 uint32_t destination_endpoint,
                                 uint32_t source_endpoint,
                                 uint32_t msg_type,
                                 uint32_t request_id,
                                 uint32_t arg0,
                                 uint32_t arg1,
                                 uint32_t arg2,
                                 uint32_t arg3,
                                 uint32_t retries)
{
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

int32_t
wasmos_sys_ipc_call_native(wasmos_driver_api_t *api,
                           uint32_t source_endpoint,
                           uint32_t destination,
                           uint32_t request_id,
                           uint32_t msg_type,
                           uint32_t arg0,
                           uint32_t arg1,
                           uint32_t arg2,
                           uint32_t arg3,
                           nd_ipc_message_t *out_message)
{
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

int32_t
wasmos_sys_svc_register_native(wasmos_driver_api_t *api,
                               uint32_t proc_endpoint,
                               uint32_t source_endpoint,
                               const uint8_t *name,
                               uint32_t name_len,
                               uint32_t request_id)
{
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

int32_t
wasmos_sys_svc_lookup_native(wasmos_driver_api_t *api,
                             uint32_t proc_endpoint,
                             uint32_t source_endpoint,
                             const uint8_t *name,
                             uint32_t name_len,
                             uint32_t request_id)
{
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

int32_t
wasmos_sys_svc_lookup_retry_native(wasmos_driver_api_t *api,
                                   uint32_t proc_endpoint,
                                   uint32_t source_endpoint,
                                   const uint8_t *name,
                                   uint32_t name_len,
                                   uint32_t request_id_base,
                                   uint32_t attempts)
{
    uint32_t i = 0;
    if (attempts == 0u) {
        attempts = 1u;
    }
    for (i = 0; i < attempts; ++i) {
        int32_t ep = wasmos_sys_svc_lookup_native(api, proc_endpoint, source_endpoint, name, name_len, request_id_base + i);
        if (ep >= 0) {
            return ep;
        }
        if (api && api->sched_yield) {
            api->sched_yield();
        }
    }
    return -1;
}

int32_t
wasmos_sys_buffer_copy_from_native(wasmos_driver_api_t *api,
                                   uint32_t kind,
                                   uint32_t source_endpoint,
                                   uint32_t borrow_flags,
                                   void *dst,
                                   int32_t len,
                                   int32_t offset)
{
    uint8_t *borrowed = 0;
    if (!api || !dst || len < 0 || offset < 0 || !api->buffer_borrow || !api->buffer_release) {
        return -1;
    }
    borrowed = (uint8_t *)api->buffer_borrow(kind, source_endpoint, borrow_flags, (uint32_t)(offset + len));
    if (!borrowed) {
        return -1;
    }
    byte_copy((uint8_t *)dst, borrowed + (uint32_t)offset, (uint32_t)len);
    if (api->buffer_release(kind) != 0) {
        return -1;
    }
    return 0;
}

int32_t
wasmos_sys_buffer_write_to_native(wasmos_driver_api_t *api,
                                  uint32_t kind,
                                  uint32_t source_endpoint,
                                  uint32_t borrow_flags,
                                  const void *src,
                                  int32_t len,
                                  int32_t offset)
{
    uint8_t *borrowed = 0;
    if (!api || !src || len < 0 || offset < 0 || !api->buffer_borrow || !api->buffer_release) {
        return -1;
    }
    borrowed = (uint8_t *)api->buffer_borrow(kind, source_endpoint, borrow_flags, (uint32_t)(offset + len));
    if (!borrowed) {
        return -1;
    }
    byte_copy(borrowed + (uint32_t)offset, (const uint8_t *)src, (uint32_t)len);
    if (api->buffer_release(kind) != 0) {
        return -1;
    }
    return 0;
}

int32_t
wasmos_sys_fs_buffer_copy_from_endpoint_native(wasmos_driver_api_t *api,
                                               uint32_t source_endpoint,
                                               void *dst,
                                               int32_t len,
                                               int32_t offset)
{
    return wasmos_sys_buffer_copy_from_native(api,
                                              ND_BUFFER_KIND_FS,
                                              source_endpoint,
                                              ND_BUFFER_BORROW_READ,
                                              dst,
                                              len,
                                              offset);
}

int32_t
wasmos_sys_fs_buffer_write_to_endpoint_native(wasmos_driver_api_t *api,
                                              uint32_t source_endpoint,
                                              const void *src,
                                              int32_t len,
                                              int32_t offset)
{
    return wasmos_sys_buffer_write_to_native(api,
                                             ND_BUFFER_KIND_FS,
                                             source_endpoint,
                                             ND_BUFFER_BORROW_WRITE,
                                             src,
                                             len,
                                             offset);
}

int32_t
wasmos_sys_fs_read_path_native(wasmos_driver_api_t *api,
                               uint32_t fs_endpoint,
                               uint32_t reply_endpoint,
                               uint32_t request_id,
                               const uint8_t *path,
                               uint32_t path_len,
                               uint8_t *out_text,
                               int32_t out_text_len)
{
    nd_ipc_message_t resp;
    int32_t read_len = 0;
    if (!api || !path || !out_text || out_text_len < 2) {
        return -1;
    }
    if (path_len == 0u) {
        return -1;
    }
    if (wasmos_sys_buffer_write_to_native(api,
                                          ND_BUFFER_KIND_FS,
                                          api->sched_current_pid ? api->sched_current_pid() : 0u,
                                          ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE,
                                          path,
                                          (int32_t)path_len,
                                          0) != 0) {
        return -1;
    }
    if (wasmos_sys_ipc_send_retry_native(api,
                                         fs_endpoint,
                                         reply_endpoint,
                                         FS_IPC_READ_PATH_REQ,
                                         request_id,
                                         path_len,
                                         0,
                                         0,
                                         0,
                                         1) != 0) {
        return -1;
    }
    if (wasmos_sys_ipc_recv_matching_native(api, reply_endpoint, request_id, &resp) != 0) {
        return -1;
    }
    if (resp.type != FS_IPC_RESP) {
        return -1;
    }
    read_len = (int32_t)resp.arg0;
    if (read_len < 0) {
        return -1;
    }
    if (read_len >= out_text_len) {
        read_len = out_text_len - 1;
    }
    if (read_len > 0 &&
        wasmos_sys_buffer_copy_from_native(api,
                                           ND_BUFFER_KIND_FS,
                                           api->sched_current_pid ? api->sched_current_pid() : 0u,
                                           ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE,
                                           out_text,
                                           read_len,
                                           0) != 0) {
        return -1;
    }
    out_text[read_len] = 0u;
    return read_len;
}
