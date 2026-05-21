#include "wasmos/libsys_native_exports.h"

void
wasmos_sys_ipc_pack_name16_zig(const uint8_t *name, uint32_t name_len, uint32_t out_args[4])
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
wasmos_sys_ipc_unpack_name16_zig(uint32_t arg0,
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
wasmos_sys_ipc_recv_loop_zig(wasmos_driver_api_t *api, uint32_t receiver_endpoint)
{
    nd_ipc_message_t msg;
    if (!api || !api->ipc_recv || !api->sched_current_pid) {
        return;
    }
    for (;;) {
        uint32_t ctx_id = api->sched_current_pid();
        int32_t rc = api->ipc_recv(ctx_id, receiver_endpoint, &msg);
        if (rc == 1) {
            if (api->sched_yield) {
                api->sched_yield();
            }
        }
    }
}

int32_t
wasmos_sys_ipc_recv_matching_zig(wasmos_driver_api_t *api,
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
wasmos_sys_ipc_send_retry_zig(wasmos_driver_api_t *api,
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
wasmos_sys_ipc_call_zig(wasmos_driver_api_t *api,
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
    return wasmos_sys_ipc_recv_matching_zig(api, source_endpoint, request_id, out_message);
}

int32_t
wasmos_sys_svc_register_zig(wasmos_driver_api_t *api,
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
    wasmos_sys_ipc_pack_name16_zig(name, name_len, args);
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
    if (wasmos_sys_ipc_recv_matching_zig(api, source_endpoint, request_id, &msg) != 0) {
        return -1;
    }
    if (msg.type != SVC_IPC_REGISTER_RESP) {
        return -1;
    }
    return (int32_t)msg.arg0;
}

int32_t
wasmos_sys_svc_lookup_zig(wasmos_driver_api_t *api,
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
    wasmos_sys_ipc_pack_name16_zig(name, name_len, args);
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
    if (wasmos_sys_ipc_recv_matching_zig(api, source_endpoint, request_id, &msg) != 0) {
        return -1;
    }
    if (msg.type != SVC_IPC_LOOKUP_RESP || msg.arg0 == 0xFFFFFFFFu) {
        return -1;
    }
    return (int32_t)msg.arg0;
}

int32_t
wasmos_sys_svc_lookup_retry_zig(wasmos_driver_api_t *api,
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
        int32_t ep = wasmos_sys_svc_lookup_zig(api,
                                               proc_endpoint,
                                               source_endpoint,
                                               name,
                                               name_len,
                                               request_id_base + i);
        if (ep >= 0) {
            return ep;
        }
        if (api && api->sched_yield) {
            api->sched_yield();
        }
    }
    return -1;
}
