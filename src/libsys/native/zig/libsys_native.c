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
