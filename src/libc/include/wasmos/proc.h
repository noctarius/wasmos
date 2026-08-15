/* proc.h - process and module metadata IPC helpers */
#ifndef WASMOS_LIBC_WASMOS_PROC_H
#define WASMOS_LIBC_WASMOS_PROC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pack a service/process name (up to 16 chars) into four int32 IPC args, 4
 * little-endian bytes per word. Longer names are truncated, shorter ones
 * zero-padded, and no terminator is transmitted. A NULL `name` yields four zero
 * words; a NULL `out_args` is a no-op. Same encoding as
 * wasmos_ipc_pack_name16. */
static inline void wasmos_proc_pack_name16(const char* name, int32_t out_args[4]) {
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

/* Query PM for module metadata by xfer path; writes path into the xfer buffer
 * before sending.  Uses a static request-id counter starting at 0x40000000, so
 * ids from this call cannot collide with those of any other request family.
 *
 * NOTE: that counter is file-static — not safe to call concurrently from
 * multiple threads without external synchronisation.
 *
 * The path is staged in a transfer buffer owned for the duration of the call and
 * released before returning; arg1 packs (buffer_id << 12 | path_len), which caps
 * path_len at 4095 here, though PM itself rejects anything from 96 bytes up.
 * `source_kind` selects the module namespace and must be
 * PROC_MODULE_SOURCE_INITFS — PM answers any other value, PROC_MODULE_SOURCE_FS
 * included, with an error.
 *
 * Blocks on `reply_endpoint` for exactly one message and accepts it only if it is
 * a PROC_IPC_RESP carrying this request's id; anything else fails the call rather
 * than being skipped and retried, so `reply_endpoint` must be private to the
 * caller. On success returns 0 and writes the module index, flags and capability
 * flags to whichever out pointers are non-NULL; returns -1 on a bad path length,
 * staging failure, send/receive failure, or a non-matching reply. */
static inline int32_t wasmos_proc_module_meta_path(int32_t proc_endpoint, int32_t reply_endpoint,
                                                   const char* path, int32_t source_kind,
                                                   int32_t* out_module_index, int32_t* out_flags,
                                                   int32_t* out_cap_flags) {
    static uint32_t request_id = 0x40000000u;
    uint32_t req = request_id++;
    size_t path_len = path ? strlen(path) : 0u;
    if (path_len == 0 || path_len > 0xFFFu || path_len >= (size_t)wasmos_xfer_buffer_size()) {
        return -1;
    }
    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)path_len);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, path), (int32_t)path_len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_ipc_send(proc_endpoint,
                        reply_endpoint,
                        PROC_IPC_MODULE_META_PATH,
                        (int32_t)req,
                        0,
                        (int32_t)(((uint32_t)bid << 12) | ((uint32_t)path_len & 0xFFFu)),
                        source_kind,
                        0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    int32_t sel = wasmos_ipc_select_one(reply_endpoint);
    (void)wasmos_xfer_buffer_release(bid);
    if (sel < 0) {
        return -1;
    }
    if ((uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != PROC_IPC_RESP) {
        return -1;
    }
    if (out_module_index)
        *out_module_index = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    if (out_flags)
        *out_flags = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    if (out_cap_flags)
        *out_cap_flags = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
