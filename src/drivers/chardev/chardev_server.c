#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, symbol_name)
#define WASMOS_WASM_EXPORT
#endif

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

static uint8_t g_last_byte;
static uint8_t g_has_data;
static int32_t g_service_endpoint = -1;

static void
chardev_reply(int32_t reply_endpoint,
              int32_t type,
              int32_t request_id,
              int32_t status,
              int32_t value)
{
    if (g_service_endpoint < 0 || reply_endpoint < 0) {
        return;
    }
    (void)wasmos_ipc_send(reply_endpoint,
                          g_service_endpoint,
                          type,
                          request_id,
                          status,
                          value,
                          0,
                          0);
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t service_endpoint,
           int32_t arg1,
           int32_t arg2,
           int32_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    g_last_byte = 0;
    g_has_data = 0;
    g_service_endpoint = service_endpoint;

    for (;;) {
        int32_t recv_rc = wasmos_ipc_recv(g_service_endpoint);
        if (recv_rc < 0) {
            continue;
        }
        int32_t msg_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t msg_req_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t msg_source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
        int32_t msg_arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);

        switch ((uint32_t)msg_type) {
            case WASM_CHARDEV_IPC_READ_REQ:
                if (!g_has_data) {
                    chardev_reply(msg_source,
                                  WASM_CHARDEV_IPC_READ_RESP,
                                  msg_req_id,
                                  -1,
                                  0);
                } else {
                    chardev_reply(msg_source,
                                  WASM_CHARDEV_IPC_READ_RESP,
                                  msg_req_id,
                                  0,
                                  (int32_t)g_last_byte);
                }
                break;
            case WASM_CHARDEV_IPC_WRITE_REQ:
                g_last_byte = (uint8_t)(msg_arg0 & 0xFF);
                g_has_data = 1;
                chardev_reply(msg_source,
                              WASM_CHARDEV_IPC_WRITE_RESP,
                              msg_req_id,
                              0,
                              msg_arg0 & 0xFF);
                break;
            default:
                chardev_reply(msg_source,
                              WASM_CHARDEV_IPC_ERROR_RESP,
                              msg_req_id,
                              -1,
                              msg_type);
                break;
        }
    }
    return 0;
}
