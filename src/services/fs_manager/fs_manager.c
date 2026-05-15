#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static int32_t g_backend_endpoint = -1;

static void
stall_forever(void)
{
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_recv(endpoint);
        }
    }
}

static void
log_msg(const char *s)
{
    if (!s) {
        return;
    }
    wasmos_console_write((int32_t)(uintptr_t)s, (int32_t)strlen(s));
}

static int
lookup_backend(void)
{
    if (g_backend_endpoint >= 0) {
        return 0;
    }
    g_backend_endpoint = wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "fs.boot", 1);
    return g_backend_endpoint >= 0 ? 0 : -1;
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t arg1,
           int32_t arg2,
           int32_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    g_proc_endpoint = proc_endpoint;
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    g_fs_endpoint = wasmos_ipc_create_endpoint();
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0 || g_fs_endpoint < 0) {
        log_msg("[fs-manager] endpoint init failed\n");
        stall_forever();
    }
    if (wasmos_svc_register(g_proc_endpoint, g_fs_endpoint, "fs", 1) != 0) {
        log_msg("[fs-manager] register fs failed\n");
        stall_forever();
    }

    for (;;) {
        if (lookup_backend() != 0) {
            (void)wasmos_sched_yield();
            continue;
        }

        if (wasmos_ipc_recv(g_fs_endpoint) < 0) {
            continue;
        }

        int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t request_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t arg1f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t arg2f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t arg3f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);

        if (wasmos_ipc_send(g_backend_endpoint,
                            g_reply_endpoint,
                            type,
                            request_id,
                            arg0,
                            arg1f,
                            arg2f,
                            arg3f) != 0) {
            (void)wasmos_ipc_send(source,
                                  g_fs_endpoint,
                                  FS_IPC_ERROR,
                                  request_id,
                                  type,
                                  0,
                                  0,
                                  0);
            continue;
        }

        if (wasmos_ipc_recv(g_reply_endpoint) < 0) {
            (void)wasmos_ipc_send(source,
                                  g_fs_endpoint,
                                  FS_IPC_ERROR,
                                  request_id,
                                  type,
                                  0,
                                  0,
                                  0);
            continue;
        }

        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t r0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t r1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t r2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t r3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);

        if (resp_req != request_id) {
            resp_type = FS_IPC_ERROR;
            r0 = type;
            r1 = r2 = r3 = 0;
        }

        (void)wasmos_ipc_send(source,
                              g_fs_endpoint,
                              resp_type,
                              request_id,
                              r0,
                              r1,
                              r2,
                              r3);
    }
}
