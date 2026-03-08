#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, import_name)
#define WASMOS_WASM_EXPORT
#endif

extern int32_t wasmos_ipc_create_endpoint(void)
    WASMOS_WASM_IMPORT("wasmos", "ipc_create_endpoint");
extern int32_t wasmos_ipc_send(int32_t destination_endpoint,
                               int32_t source_endpoint,
                               int32_t type,
                               int32_t request_id,
                               int32_t arg0,
                               int32_t arg1)
    WASMOS_WASM_IMPORT("wasmos", "ipc_send");
extern int32_t wasmos_ipc_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_recv");
extern int32_t wasmos_ipc_last_field(int32_t field)
    WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");

typedef enum {
    INIT_PHASE_START = 0,
    INIT_PHASE_WAIT_SPAWN,
    INIT_PHASE_DONE,
    INIT_PHASE_FAILED
} init_phase_t;

static init_phase_t g_phase = INIT_PHASE_START;
static int32_t g_reply_endpoint = -1;
static int32_t g_spawn_request_id = 1;

WASMOS_WASM_EXPORT int32_t
init_step(int32_t ignored_type,
          int32_t proc_endpoint,
          int32_t chardev_module_index,
          int32_t ignored_arg2,
          int32_t ignored_arg3)
{
    (void)ignored_type;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (g_phase == INIT_PHASE_START) {
        g_reply_endpoint = wasmos_ipc_create_endpoint();
        if (g_reply_endpoint < 0) {
            g_phase = INIT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        if (proc_endpoint < 0 || chardev_module_index < 0) {
            g_phase = INIT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        if (wasmos_ipc_send(proc_endpoint, g_reply_endpoint,
                            PROC_IPC_SPAWN,
                            g_spawn_request_id,
                            chardev_module_index,
                            0) != 0) {
            g_phase = INIT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        g_phase = INIT_PHASE_WAIT_SPAWN;
        return WASMOS_WASM_STEP_YIELDED;
    }

    if (g_phase == INIT_PHASE_WAIT_SPAWN) {
        int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
        if (recv_rc == 0) {
            return WASMOS_WASM_STEP_BLOCKED;
        }
        if (recv_rc < 0) {
            g_phase = INIT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != g_spawn_request_id) {
            g_phase = INIT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        if (resp_type == PROC_IPC_ERROR) {
            g_phase = INIT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }
        if (resp_type != PROC_IPC_RESP) {
            g_phase = INIT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        g_phase = INIT_PHASE_DONE;
        return WASMOS_WASM_STEP_DONE;
    }

    if (g_phase == INIT_PHASE_DONE) {
        return WASMOS_WASM_STEP_DONE;
    }

    return WASMOS_WASM_STEP_FAILED;
}
