/* startup.c - WASM process entry point and per-context managed IPC state */
#include "wasmos/startup.h"
#include "wasmos/imports.h"
#include "wasmos/api.h"

static int32_t g_wasmos_startup_args[4];
static char *g_wasmos_argv[1];
static int32_t g_wasmos_ipc_reply_endpoint = -1;
static int32_t g_wasmos_ipc_request_id = 1;

int32_t
wasmos_startup_arg(uint32_t index)
{
    if (index >= 4u) {
        return 0;
    }
    return g_wasmos_startup_args[index];
}

/* Return (creating if necessary) the per-context managed reply endpoint.
 * Lazy creation avoids wasting endpoints in processes that never use IPC. */
int32_t
wasmos_ipc_ensure_reply_endpoint(void)
{
    if (g_wasmos_ipc_reply_endpoint >= 0) {
        return g_wasmos_ipc_reply_endpoint;
    }
    g_wasmos_ipc_reply_endpoint = wasmos_ipc_create_endpoint();
    return g_wasmos_ipc_reply_endpoint;
}

int32_t
wasmos_ipc_next_request_id(void)
{
    int32_t id = g_wasmos_ipc_request_id++;
    if (g_wasmos_ipc_request_id < 1) {
        g_wasmos_ipc_request_id = 1;
    }
    return id;
}

extern int main(int argc, char **argv);

/* WASM export called by PM instead of _start; stores the four startup args
 * (proc_endpoint, home_tty, reserved, reserved) then calls main(0, argv). */
WASMOS_WASM_EXPORT int32_t
wasmos_main(int32_t arg0,
            int32_t arg1,
            int32_t arg2,
            int32_t arg3)
{
    g_wasmos_startup_args[0] = arg0;
    g_wasmos_startup_args[1] = arg1;
    g_wasmos_startup_args[2] = arg2;
    g_wasmos_startup_args[3] = arg3;
    g_wasmos_argv[0] = 0;
    int32_t rc = main(0, g_wasmos_argv);
    (void)wasmos_proc_exit(rc);
    return rc;
}
