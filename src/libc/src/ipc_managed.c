/* ipc_managed.c - per-context managed IPC reply endpoint helpers */
#include "wasmos/api.h"

static int32_t g_wasmos_ipc_reply_endpoint = -1;
static int32_t g_wasmos_ipc_request_id = 1;

int32_t wasmos_ipc_ensure_reply_endpoint(void) {
    if (g_wasmos_ipc_reply_endpoint >= 0) {
        return g_wasmos_ipc_reply_endpoint;
    }
    g_wasmos_ipc_reply_endpoint = wasmos_ipc_create_endpoint();
    return g_wasmos_ipc_reply_endpoint;
}

int32_t wasmos_ipc_next_request_id(void) {
    int32_t id = g_wasmos_ipc_request_id++;
    if (g_wasmos_ipc_request_id < 1) {
        g_wasmos_ipc_request_id = 1;
    }
    return id;
}
