#ifndef WASMOS_LIBC_WASMOS_PROC_H
#define WASMOS_LIBC_WASMOS_PROC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

static inline void
wasmos_proc_pack_name16(const char *name, int32_t out_args[4])
{
    uint32_t packed[4] = {0, 0, 0, 0};
    if (!out_args) {
        return;
    }
    if (name) {
        for (uint32_t i = 0; name[i] && i < 16u; ++i) {
            uint32_t slot = i / 4u;
            uint32_t shift = (i % 4u) * 8u;
            packed[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
        }
    }
    out_args[0] = (int32_t)packed[0];
    out_args[1] = (int32_t)packed[1];
    out_args[2] = (int32_t)packed[2];
    out_args[3] = (int32_t)packed[3];
}

static inline int32_t
wasmos_proc_module_meta(int32_t proc_endpoint,
                        int32_t reply_endpoint,
                        int32_t module_index,
                        int32_t match_index,
                        int32_t *out_arg0,
                        int32_t *out_arg1,
                        int32_t *out_arg2,
                        int32_t *out_arg3)
{
    static uint32_t request_id = 1u;
    uint32_t req = request_id++;
    if (wasmos_ipc_send(proc_endpoint,
                        reply_endpoint,
                        PROC_IPC_MODULE_META,
                        (int32_t)req,
                        module_index,
                        match_index,
                        0,
                        0) != 0) {
        return -1;
    }
    if (wasmos_ipc_recv(reply_endpoint) < 0) {
        return -1;
    }
    if ((uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        return -1;
    }
    if (out_arg0) *out_arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (out_arg1) *out_arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    if (out_arg2) *out_arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    if (out_arg3) *out_arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
    return 0;
}

static inline int32_t
wasmos_proc_module_meta_path(int32_t proc_endpoint,
                             int32_t reply_endpoint,
                             const char *path,
                             int32_t source_kind,
                             int32_t *out_module_index,
                             int32_t *out_flags,
                             int32_t *out_cap_flags)
{
    static uint32_t request_id = 0x40000000u;
    uint32_t req = request_id++;
    size_t path_len = path ? strlen(path) : 0u;
    if (path_len == 0 || path_len >= (size_t)wasmos_fs_buffer_size()) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    if (wasmos_ipc_send(proc_endpoint,
                        reply_endpoint,
                        PROC_IPC_MODULE_META_PATH,
                        (int32_t)req,
                        0,
                        (int32_t)path_len,
                        source_kind,
                        0) != 0) {
        return -1;
    }
    if (wasmos_ipc_recv(reply_endpoint) < 0) {
        return -1;
    }
    if ((uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        return -1;
    }
    if (out_module_index) *out_module_index = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (out_flags) *out_flags = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    if (out_cap_flags) *out_cap_flags = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    return 0;
}

#endif
