#ifndef WASMOS_LIBSYS_H
#define WASMOS_LIBSYS_H

#include <stdint.h>

#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline int32_t
wasmos_sys_recv_matching(int32_t reply_endpoint,
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
    if (wasmos_sys_recv_matching(reply_endpoint, request_id, &resp) != 0) {
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

#ifdef __cplusplus
}
#endif

#endif
