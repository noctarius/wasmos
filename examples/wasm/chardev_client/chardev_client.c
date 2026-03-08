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
                               int32_t arg1,
                               int32_t arg2,
                               int32_t arg3)
    WASMOS_WASM_IMPORT("wasmos", "ipc_send");
extern int32_t wasmos_ipc_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_recv");
extern int32_t wasmos_ipc_last_field(int32_t field)
    WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");

typedef enum {
    CLIENT_PHASE_INIT = 0,
    CLIENT_PHASE_WAIT_WRITE,
    CLIENT_PHASE_WAIT_READ,
    CLIENT_PHASE_DONE,
    CLIENT_PHASE_FAILED
} client_phase_t;

static client_phase_t g_phase = CLIENT_PHASE_INIT;
static int32_t g_reply_endpoint = -1;
static int32_t g_write_request_id = 1;
static int32_t g_read_request_id = 2;
static int32_t g_write_value = 0x41;

WASMOS_WASM_EXPORT int32_t
chardev_client_step(int32_t ignored_type,
                    int32_t chardev_endpoint,
                    int32_t ignored_arg1,
                    int32_t ignored_arg2,
                    int32_t ignored_arg3)
{
    (void)ignored_type;
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (g_phase == CLIENT_PHASE_INIT) {
        g_reply_endpoint = wasmos_ipc_create_endpoint();
        if (g_reply_endpoint < 0) {
            g_phase = CLIENT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        if (wasmos_ipc_send(chardev_endpoint, g_reply_endpoint,
                            WASM_CHARDEV_IPC_WRITE_REQ,
                            g_write_request_id, g_write_value, 0, 0, 0) != 0) {
            g_phase = CLIENT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        g_phase = CLIENT_PHASE_WAIT_WRITE;
        return WASMOS_WASM_STEP_YIELDED;
    }

    if (g_phase == CLIENT_PHASE_WAIT_WRITE) {
        int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
        if (recv_rc == 0) {
            return WASMOS_WASM_STEP_BLOCKED;
        }
        if (recv_rc < 0) {
            g_phase = CLIENT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t resp_value = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        if (resp_type != WASM_CHARDEV_IPC_WRITE_RESP
            || resp_req != g_write_request_id
            || resp_status != 0
            || (resp_value & 0xFF) != (g_write_value & 0xFF)) {
            g_phase = CLIENT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        if (wasmos_ipc_send(chardev_endpoint, g_reply_endpoint,
                            WASM_CHARDEV_IPC_READ_REQ,
                            g_read_request_id, 0, 0, 0, 0) != 0) {
            g_phase = CLIENT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        g_phase = CLIENT_PHASE_WAIT_READ;
        return WASMOS_WASM_STEP_YIELDED;
    }

    if (g_phase == CLIENT_PHASE_WAIT_READ) {
        int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
        if (recv_rc == 0) {
            return WASMOS_WASM_STEP_BLOCKED;
        }
        if (recv_rc < 0) {
            g_phase = CLIENT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t resp_value = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        if (resp_type != WASM_CHARDEV_IPC_READ_RESP
            || resp_req != g_read_request_id
            || resp_status != 0
            || (resp_value & 0xFF) != (g_write_value & 0xFF)) {
            g_phase = CLIENT_PHASE_FAILED;
            return WASMOS_WASM_STEP_FAILED;
        }

        g_phase = CLIENT_PHASE_DONE;
        return WASMOS_WASM_STEP_DONE;
    }

    if (g_phase == CLIENT_PHASE_DONE) {
        return WASMOS_WASM_STEP_DONE;
    }

    return WASMOS_WASM_STEP_FAILED;
}
